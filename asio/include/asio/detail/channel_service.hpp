//
// detail/channel_service.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2013 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_DETAIL_CHANNEL_SERVICE_HPP
#define ASIO_DETAIL_CHANNEL_SERVICE_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "asio/detail/config.hpp"
#include <deque>
#include "asio/error.hpp"
#include "asio/io_service.hpp"
#include "asio/detail/addressof.hpp"
#include "asio/detail/channel_get_op.hpp"
#include "asio/detail/channel_op.hpp"
#include "asio/detail/channel_put_op.hpp"
#include "asio/detail/handler_alloc_helpers.hpp"
#include "asio/detail/handler_cont_helpers.hpp"
#include "asio/detail/mutex.hpp"
#include "asio/detail/op_queue.hpp"
#include "asio/detail/operation.hpp"

#include "asio/detail/push_options.hpp"

namespace asio {
namespace detail {

class channel_service
  : public asio::detail::service_base<channel_service>
{
public:
  // The base implementation type of all channels.
  struct base_implementation_type
  {
    // Default constructor.
    base_implementation_type()
      : open_(true),
        max_buffer_size_(0),
        next_(0),
        prev_(0)
    {
    }

    // Whether the channel is currently open.
    bool open_;

    // The maximum number of elements that may be buffered in the channel.
    std::size_t max_buffer_size_;

    // The operations that are waiting on the channel.
    op_queue<operation> putters_;
    op_queue<operation> getters_;

    // Pointers to adjacent channel implementations in linked list.
    base_implementation_type* next_;
    base_implementation_type* prev_;
  };

  // The implementation for a specific value type.
  template <typename T>
  struct implementation_type : base_implementation_type
  {
    // Buffered values.
    std::deque<T> buffer_;
  };

  // The implementation for a void value type.
  struct void_implementation_type : base_implementation_type
  {
    // Number of buffered "values".
    std::size_t buffered_;
  };

  // Constructor.
  ASIO_DECL channel_service(
      asio::io_service& io_service);

  // Destroy all user-defined handler objects owned by the service.
  ASIO_DECL void shutdown_service();

  // Construct a new channel implementation.
  ASIO_DECL void construct(base_implementation_type&,
      std::size_t max_buffer_size);

  // Destroy a channel implementation.
  ASIO_DECL void destroy(base_implementation_type& impl);

  // Determine whether the channel is open.
  bool is_open(const base_implementation_type& impl) const
  {
    return impl.open_;
  }

  // Open the channel.
  void open(base_implementation_type& impl)
  {
    impl.open_ = true;
  }

  // Close the channel.
  ASIO_DECL void close(base_implementation_type& impl);

  // Cancel all operations associated with the channel.
  ASIO_DECL void cancel(base_implementation_type& impl);

  // Determine whether a value can be read from the channel without blocking.
  template <typename T>
  bool ready(const implementation_type<T>& impl) const
  {
    return (!impl.buffer_.empty() || !impl.putters_.empty());
  }

  // Determine whether a value can be read from the channel without blocking.
  bool ready(const void_implementation_type& impl) const
  {
    return (impl.buffered_ > 0 || !impl.putters_.empty());
  }

  // Synchronously place a new value into the channel.
  template <typename T, typename T0>
  void put(implementation_type<T>& impl,
      ASIO_MOVE_ARG(T0) value, asio::error_code& ec);

  // Asynchronously place a new value into the channel.
  template <typename T, typename T0, typename Handler>
  void async_put(implementation_type<T>& impl,
      ASIO_MOVE_ARG(T0) value, Handler& handler)
  {
    bool is_continuation =
      asio_handler_cont_helpers::is_continuation(handler);

    // Allocate and construct an operation to wrap the handler.
    typedef channel_put_op<T, Handler> op;
    typename op::ptr p = { asio::detail::addressof(handler),
      asio_handler_alloc_helpers::allocate(
        sizeof(op), handler), 0 };
    p.p = new (p.v) op(ASIO_MOVE_CAST(T0)(value), handler);

    ASIO_HANDLER_CREATION((p.p, "channel", this, "async_put"));

    do_put(impl, p.p, is_continuation);
    p.v = p.p = 0;
  }

  // Synchronously remove a value from the channel.
  template <typename T>
  T get(implementation_type<T>& impl, asio::error_code& ec);

  // Asynchronously remove a value from the channel.
  template <typename T, typename Handler>
  void async_get(implementation_type<T>& impl, Handler& handler)
  {
    bool is_continuation =
      asio_handler_cont_helpers::is_continuation(handler);

    // Allocate and construct an operation to wrap the handler.
    typedef channel_get_op<T, Handler> op;
    typename op::ptr p = { asio::detail::addressof(handler),
      asio_handler_alloc_helpers::allocate(
        sizeof(op), handler), 0 };
    p.p = new (p.v) op(handler);

    ASIO_HANDLER_CREATION((p.p, "channel", this, "async_get"));

    do_get(impl, p.p, is_continuation);
    p.v = p.p = 0;
  }

private:
  template <typename T>
  void do_put(implementation_type<T>& impl,
      channel_op<T>* putter, bool is_continuation)
  {
    if (!impl.open_)
    {
      putter->ec_ = asio::error::broken_pipe;
      io_service_.post_immediate_completion(putter, is_continuation);
    }
    else if (channel_op<T>* getter =
        static_cast<channel_op<T>*>(impl.getters_.front()))
    {
      getter->set_value(ASIO_MOVE_CAST(T)(putter->get_value()));
      impl.getters_.pop();
      io_service_.post_deferred_completion(getter);
      io_service_.post_immediate_completion(putter, is_continuation);
    }
    else
    {
      if (impl.buffer_.size() < impl.max_buffer_size_)
      {
        impl.buffer_.resize(impl.buffer_.size() + 1);
        impl.buffer_.back() = ASIO_MOVE_CAST(T)(putter->get_value());
        io_service_.post_immediate_completion(putter, is_continuation);
      }
      else
      {
        impl.putters_.push(putter);
        io_service_.work_started();
      }
    }
  }

  template <typename T>
  void do_get(implementation_type<T>& impl,
      channel_op<T>* getter, bool is_continuation)
  {
    if (!impl.buffer_.empty())
    {
      getter->set_value(ASIO_MOVE_CAST(T)(impl.buffer_.front()));
      impl.buffer_.pop_front();
      if (channel_op<T>* putter =
          static_cast<channel_op<T>*>(impl.putters_.front()))
      {
        impl.buffer_.resize(impl.buffer_.size() + 1);
        impl.buffer_.back() = ASIO_MOVE_CAST(T)(putter->get_value());
        impl.putters_.pop();
        io_service_.post_deferred_completion(putter);
      }
      io_service_.post_immediate_completion(getter, is_continuation);
    }
    else if (channel_op<T>* putter =
        static_cast<channel_op<T>*>(impl.putters_.front()))
    {
      getter->set_value(ASIO_MOVE_CAST(T)(putter->get_value()));
      impl.putters_.pop();
      io_service_.post_deferred_completion(putter);
      io_service_.post_immediate_completion(getter, is_continuation);
    }
    else if (impl.open_)
    {
      impl.getters_.push(getter);
      io_service_.work_started();
    }
    else
    {
      getter->ec_ = asio::error::broken_pipe;
      io_service_.post_immediate_completion(getter, is_continuation);
    }
  }

  // The io_service implementation used for delivering completions.
  io_service_impl& io_service_;

  // Mutex to protect access to the linked list of implementations. 
  asio::detail::mutex mutex_;

  // The head of a linked list of all implementations.
  base_implementation_type* impl_list_;
};

} // namespace detail
} // namespace asio

#include "asio/detail/pop_options.hpp"

#include "asio/detail/impl/channel_service.hpp"
#if defined(ASIO_HEADER_ONLY)
# include "asio/detail/impl/channel_service.ipp"
#endif // defined(ASIO_HEADER_ONLY)

#endif // ASIO_DETAIL_CHANNEL_SERVICE_HPP