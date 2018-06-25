#pragma once

/* 
 * Presents a half-open interval [a,b) of integral values as a container.
 */

#include <type_traits>
#include <utility>

#include <util/counter.hpp>
#include <util/deduce_return.hpp>
#include <util/meta.hpp>
#include <util/range.hpp>

namespace arb {
namespace util {

// TODO: simplify span-using code by:
// 1. replace type alias `span` with `span_type` alias;
// 2. rename `make_span` as `span`
// 3. add another `span(I n)` overload equivalent to `span(I{}, n)`.

template <typename I>
using span = range<counter<I>>;

template <typename I, typename J>
span<typename std::common_type<I, J>::type> make_span(I left, J right) {
    return span<typename std::common_type<I, J>::type>(left, right);
}

template <typename I, typename J>
span<typename std::common_type<I, J>::type> make_span(std::pair<I, J> interval) {
    return span<typename std::common_type<I, J>::type>(interval.first, interval.second);
}

template <typename I>
span<I> make_span(I right) {
    return span<I>(I{}, right);
}

template <typename Seq>
auto count_along(const Seq& s) DEDUCED_RETURN_TYPE(util::make_span(util::size(s)))

} // namespace util
} // namespace arb