/*
 * Copyright 2020 Vectorized, Inc.
 *
 * Use of this software is governed by the Business Source License
 * included in the file licenses/BSL.md
 *
 * As of the Change Date specified in that file, in accordance with
 * the Business Source License, use of this software will be governed
 * by the Apache License, Version 2.0
 */

#pragma once
#include "utils/concepts-enabled.h"
#include "utils/functional.h"

#include <seastar/core/future-util.hh>
#include <seastar/core/future.hh>

#include <algorithm>
#include <iterator>

namespace ssx {

/// \brief Run tasks synchronously in order and wait for completion only
/// invoking futures one after the previous has completed
///
/// Given a range [\c begin, \c end) of objects, run \c func on each \c *i in
/// the range, and return a \c future<> containing a \c std::vector<> of values.
/// In case any of the given tasks fails, one of the exceptions is returned by
/// this function as a failed future.
///
/// \param begin an \c InputIterator designating the beginning of the range
/// \param end an \c InputIterator designating the end of the range
/// \param func Function to invoke with each element in the range (will be
/// futurized if it doesn't return a \c future<>)
/// \return a \c future<> containing a \c std::vector<> of the results of the
/// function invocations that resolves when all the function invocations
/// complete.  If one or more return an exception, the return value contains one
/// of the exceptions.
// clang-format off
template<typename Iterator, typename Func>
CONCEPT(requires requires(Func f, Iterator i) {
    *i++;
    { i != i } -> std::convertible_to<bool>;
    seastar::futurize_invoke(f, *i).get0();
})
// clang-format on
inline auto async_transform(Iterator begin, Iterator end, Func&& func) {
    using result_type = decltype(seastar::futurize_invoke(func, *begin).get0());
    std::vector<result_type> res;
    res.reserve(std::distance(begin, end));
    return seastar::do_with(
      std::move(res),
      std::move(begin),
      std::move(end),
      [func{std::forward<Func>(func)}](
        std::vector<result_type>& res, Iterator& begin, Iterator& end) mutable {
          return seastar::do_for_each(
                   begin,
                   end,
                   [&res, func{std::forward<Func>(func)}](auto&& val) mutable {
                       return seastar::futurize_invoke(
                                func, std::forward<decltype(val)>(val))
                         .then([&res](result_type r) {
                             return res.push_back(std::move(r));
                         });
                   })
            .then([&res] { return std::move(res); });
      });
}

/// \brief Run tasks synchronously in order and wait for completion only
/// invoking futures one after a previous has completed (range version)
///
/// Given a range \c rng of objects, run \c func on each element in the range,
/// and return a \c future<> containing a \c std::vector<> of values. In case
/// any of the given tasks fails, one of the exceptions is returned by this
/// function as a failed future.
///
/// If \c rng is an rvalue reference, it will be kept alive.
///
/// \param rng an \c InputRange
/// \param func Function to invoke with each element in the range (will be
/// futurized if it doesn't return a \c future<>)
/// \return a \c future<> containing a \c std::vector<> of the results of the
/// function invocations that resolves when all the function invocations
/// complete.  If one or more return an exception, the return value contains one
/// of the exceptions.
// clang-format off
template<typename Rng, typename Func>
CONCEPT(requires requires(Func f, Rng r) {
    r.begin();
    r.end();
    { r.begin() != r.begin() } -> std::convertible_to<bool>;
    seastar::futurize_invoke(f, *r.begin()).get0();
})
// clang-format on
inline auto async_transform(Rng&& rng, Func&& func) {
    return async_transform(rng.begin(), rng.end(), std::forward<Func>(func));
}

/// \brief Run tasks synchronously in order and wait for completion only
/// invoking futures one after a previous has completed (range version).
/// Difference is that the results are expected to be a series of collections
/// for which are then transformed into one single collection.
///
/// Given a range [\c begin, \c end) of objects, run \c func on each \c *i in
/// the range, and return a \c future<> containing a \c std::vector<> of
/// std::vector<> of values. These values are then unflattened into a single
/// vector as the functions return value. In case any of the given tasks fails,
/// one of the exceptions is returned by this function as a failed future.
///
/// \param begin an \c InputIterator designating the beginning of the range
/// \param end an \c InputIterator designating the end of the range
/// \param func Function to invoke with each element in the range (will be
/// futurized if it doesn't return a \c future<> and expected to return a
/// std::vector<> of values)
/// \return a \c future<> containing a \c std::vector<>
/// of the results of the function invocations that resolves when all the
/// function invocations complete.  If one or more return an exception, the
/// return value contains one of the exceptions.
// clang-format off
template<typename Iterator, typename Func>
CONCEPT(requires requires(Func f, Iterator i) {
    *i++;
    { i != i } ->std::convertible_to<bool>;
    seastar::futurize_invoke(f, *i).get0().begin();
    seastar::futurize_invoke(f, *i).get0().end();
})
// clang-format on
inline auto async_flat_transform(Iterator begin, Iterator end, Func&& func) {
    using result_type = decltype(seastar::futurize_invoke(func, *begin).get0());
    using value_type = typename result_type::value_type;
    std::vector<value_type> res;
    res.reserve(std::distance(begin, end));
    return seastar::do_with(
      std::move(res),
      std::move(begin),
      std::move(end),
      [func{std::forward<Func>(func)}](
        std::vector<value_type>& res, Iterator& begin, Iterator& end) mutable {
          return seastar::do_for_each(
                   begin,
                   end,
                   [&res, func{std::forward<Func>(func)}](auto&& val) mutable {
                       return seastar::futurize_invoke(
                                func, std::forward<decltype(val)>(val))
                         .then([&res](std::vector<value_type> r) {
                             return seastar::do_with(
                               std::move(r),
                               [&res](std::vector<value_type>& r) {
                                   return seastar::do_for_each(
                                     r, [&res](value_type& element) {
                                         res.push_back(std::move(element));
                                     });
                               });
                         });
                   })
            .then([&res] { return std::move(res); });
      });
}

// clang-format off
template<typename Rng, typename Func>
CONCEPT(requires requires(Func f, Rng r) {
    r.begin();
    r.end();
    { r.begin() != r.begin() } -> std::convertible_to<bool>;
    seastar::futurize_invoke(f, *r.begin()).get0();
})
// clang-format on
inline auto async_flat_transform(Rng& rng, Func&& func) {
    return async_flat_transform(
      rng.begin(), rng.end(), std::forward<Func>(func));
}

/// \brief Run tasks in parallel and wait for completion, capturing possible
/// errors (iterator version).
///
/// Given a range [\c begin, \c end) of objects, run \c func on each \c *i in
/// the range, and return a \c future<> containing a \c std::vector<> of
/// values. In case any of the given tasks fails, one of the exceptions is
/// returned by this function as a failed future.
///
/// \param begin an \c InputIterator designating the beginning of the range
/// \param end an \c InputIterator designating the end of the range
/// \param func Function to invoke with each element in the range (will be
/// futurized if it doesn't return a \c future<>)
/// \return a \c future<> containing a \c std::vector<> of the results of the
/// function invocations that resolves when all the function invocations
/// complete.  If one or more return an exception, the return value contains one
/// of the exceptions.
// clang-format off
template<typename Iterator, typename Func>
CONCEPT(requires requires(Func f, Iterator i) {
    *i++;
    { i != i } -> std::convertible_to<bool>;
})
// clang-format on
inline auto parallel_transform(Iterator begin, Iterator end, Func func) {
    using value_type = typename std::iterator_traits<Iterator>::value_type;
    using future = decltype(
      seastar::futurize_invoke(std::move(func), std::move(*begin)));
    std::vector<future> res;
    res.reserve(std::distance(begin, end));
    std::transform(
      begin,
      end,
      std::back_inserter(res),
      [func{std::move(func)}](value_type val) mutable {
          return seastar::futurize_invoke(std::move(func), std::move(val));
      });
    return seastar::do_with(std::move(res), [](std::vector<future>& res) {
        return seastar::when_all_succeed(
          std::make_move_iterator(res.begin()),
          std::make_move_iterator(res.end()));
    });
}

/// \brief Run tasks in parallel and wait for completion, capturing possible
/// errors (range version).
///
/// Given a range \c rng of objects, run \c func on each element in the range,
/// and return a \c future<> containing a \c std::vector<> of values. In case
/// any of the given tasks fails, one of the exceptions is returned by this
/// function as a failed future.
///
/// If \c rng is an rvalue reference, it will be kept alive.
///
/// \param rng an \c InputRange
/// \param func Function to invoke with each element in the range (will be
/// futurized if it doesn't return a \c future<>)
/// \return a \c future<> containing a \c std::vector<> of the results of the
/// function invocations that resolves when all the function invocations
/// complete.  If one or more return an exception, the return value contains one
/// of the exceptions.
// clang-format off
template<typename Rng, typename Func>
CONCEPT(requires requires(Func f, Rng r) {
    r.begin();
    r.end();
    { r.begin() != r.begin() } -> std::convertible_to<bool>;
})
// clang-format on
inline auto parallel_transform(Rng rng, Func func) {
    return seastar::do_with(
      std::move(rng), [func{std::move(func)}](Rng& rng) mutable {
          return parallel_transform(
            std::make_move_iterator(rng.begin()),
            std::make_move_iterator(rng.end()),
            std::move(func));
      });
}

/// \brief Specialization of the ssx::async_transform method. Specifically this
/// method expects a Mapper to return a bool. After calling async_transform and
/// recieving a std::vector<bool> this method returns true if all futures
/// resolved to 'true'
// clang-format off
template<typename Iterator, typename Func>
CONCEPT(requires requires(Func f, Iterator i) {
    { seastar::futurize_invoke(f, *i) } -> std::same_as<ss::future<bool>>;
})
// clang-format on
inline seastar::future<bool> async_all_of(
  Iterator begin, Iterator end, Func&& func) {
    return async_transform(
             std::move(begin), std::move(end), std::forward<Func>(func))
      .then([](const std::vector<bool>& results) {
          return std::all_of(
            results.cbegin(), results.cend(), xform::logical_true());
      });
}

} // namespace ssx
