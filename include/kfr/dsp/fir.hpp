/** @addtogroup fir
 *  @{
 */
/*
  Copyright (C) 2016-2023 Dan Cazarin (https://www.kfrlib.com)
  This file is part of KFR

  KFR is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  KFR is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with KFR.

  If GPL is not suitable for your project, you must purchase a commercial license to use KFR.
  Buying a commercial license is mandatory as soon as you develop commercial activities without
  disclosing the source code of your own applications.
  See https://www.kfrlib.com for details.
 */
#pragma once

#include "../base/basic_expressions.hpp"
#include "../base/filter.hpp"
#include "../base/memory.hpp"
#include "../base/reduce.hpp"
#include "../base/simd_expressions.hpp"
#include "../base/state_holder.hpp"
#include "../base/univector.hpp"
#include "../simd/vec.hpp"

CMT_PRAGMA_MSVC(warning(push))
CMT_PRAGMA_MSVC(warning(disable : 4244))

namespace kfr
{
inline namespace CMT_ARCH_NAME
{

template <typename T, size_t Size>
using fir_taps = univector<T, Size>;

template <size_t tapcount, typename T, typename U = T>
struct short_fir_state
{
    template <size_t N>
    short_fir_state(const univector<T, N>& taps)
        : taps(widen<tapcount>(read<N>(taps.data()), T(0))), delayline(0)
    {
    }
    template <size_t N>
    short_fir_state(const univector<const T, N>& taps)
        : taps(widen<tapcount>(read<N>(taps.data()), T(0))), delayline(0)
    {
    }
    vec<T, tapcount> taps;
    mutable vec<U, tapcount - 1> delayline;
};

template <typename T, typename U = T>
struct fir_state
{
    fir_state(const array_ref<const T>& taps)
        : taps(taps.size()), delayline(taps.size(), U(0)), delayline_cursor(0)
    {
        this->taps = reverse(make_univector(taps.data(), taps.size()));
    }
    univector<T> taps;
    mutable univector<U> delayline;
    mutable size_t delayline_cursor;
};

template <typename U, univector_tag Tag = tag_dynamic_vector>
struct moving_sum_state
{
    moving_sum_state() : delayline({ 0 }), head_cursor(0), tail_cursor(1) {}
    mutable univector<U, Tag> delayline;
    mutable size_t head_cursor, tail_cursor;
};
template <typename U>
struct moving_sum_state<U, tag_dynamic_vector>
{
    moving_sum_state(size_t sum_length) : delayline(sum_length, U(0)), head_cursor(0), tail_cursor(1) {}
    mutable univector<U> delayline;
    mutable size_t head_cursor, tail_cursor;
};

template <size_t tapcount, typename T, typename U, typename E1, bool stateless = false>
struct expression_short_fir : expression_with_traits<E1>
{
    using value_type = U; // override value_type

    static_assert(expression_traits<E1>::dims == 1, "expression_short_fir requires input with dims == 1");
    constexpr static inline bool random_access = false;

    expression_short_fir(E1&& e1, const short_fir_state<tapcount, T, U>& state)
        : expression_with_traits<E1>(std::forward<E1>(e1)), state(state)
    {
    }

    template <size_t N>
    KFR_INTRINSIC friend vec<U, N> get_elements(const expression_short_fir& self, shape<1> index,
                                                axis_params<0, N> sh)
    {
        vec<U, N> in = get_elements(self.first(), index, sh);

        vec<U, N> out = in * self.state->taps.front();
        cforeach(csizeseq<tapcount - 1, 1>,
                 [&](auto I) {
                     out = out + concat_and_slice<tapcount - 1 - I, N>(self.state->delayline, in) *
                                     self.state->taps[I];
                 });
        self.state->delayline = concat_and_slice<N, tapcount - 1>(self.state->delayline, in);

        return out;
    }
    state_holder<const short_fir_state<tapcount, T, U>, stateless> state;
};

template <typename T, typename U, typename E1, bool stateless = false>
struct expression_fir : expression_with_traits<E1>
{
    using value_type = U; // override value_type

    static_assert(expression_traits<E1>::dims == 1, "expression_fir requires input with dims == 1");
    constexpr static inline bool random_access = false;

    expression_fir(E1&& e1, const fir_state<T, U>& state)
        : expression_with_traits<E1>(std::forward<E1>(e1)), state(state)
    {
    }

    template <size_t N>
    KFR_INTRINSIC friend vec<U, N> get_elements(const expression_fir& self, shape<1> index,
                                                axis_params<0, N> sh)
    {
        const size_t tapcount = self.state->taps.size();
        const vec<U, N> input = get_elements(self.first(), index, sh);

        vec<U, N> output;
        size_t cursor = self.state->delayline_cursor;
        CMT_LOOP_NOUNROLL
        for (size_t i = 0; i < N; i++)
        {
            self.state->delayline.ringbuf_write(cursor, input[i]);
            U v =
                dotproduct(self.state->taps.slice(0, tapcount - cursor), self.state->delayline.slice(cursor));
            if (cursor > 0)
                v = v + dotproduct(self.state->taps.slice(tapcount - cursor),
                                   self.state->delayline.slice(0, cursor));
            output[i] = v;
        }
        self.state->delayline_cursor = cursor;
        return output;
    }
    state_holder<const fir_state<T, U>, stateless> state;
};

template <typename U, typename E1, univector_tag STag, bool stateless = false>
struct expression_moving_sum : expression_with_traits<E1>
{
    using value_type = U; // override value_type

    static_assert(expression_traits<E1>::dims == 1, "expression_moving_sum requires input with dims == 1");
    constexpr static inline bool random_access = false;

    expression_moving_sum(E1&& e1, const moving_sum_state<U, STag>& state)
        : expression_with_traits<E1>(std::forward<E1>(e1)), state(state)
    {
    }

    template <size_t N>
    KFR_INTRINSIC friend vec<U, N> get_elements(const expression_moving_sum& self, shape<1> index,
                                                axis_params<0, N> sh)
    {
        const vec<U, N> input = get_elements(self.first(), index, sh);

        vec<U, N> output;
        size_t wcursor = self.state->head_cursor;
        size_t rcursor = self.state->tail_cursor;

        // initial summation
        self.state->delayline.ringbuf_write(wcursor, input[0]);
        auto s    = sum(self.state->delayline);
        output[0] = s;

        CMT_LOOP_NOUNROLL
        for (size_t i = 1; i < N; i++)
        {
            U nextout;
            self.state->delayline.ringbuf_read(rcursor, nextout);
            const U nextin = input[i];
            self.state->delayline.ringbuf_write(wcursor, nextin);
            s += nextin - nextout;
            output[i] = s;
        }
        self.state->delayline.ringbuf_step(rcursor, 1);
        self.state->head_cursor = wcursor;
        self.state->tail_cursor = rcursor;
        return output;
    }
    state_holder<const moving_sum_state<U, STag>, stateless> state;
};

/**
 * @brief Returns template expression that applies FIR filter to the input
 * @param e1 an input expression
 * @param taps coefficients for the FIR filter
 */
template <typename T, typename E1, univector_tag Tag>
KFR_INTRINSIC expression_fir<T, expression_value_type<E1>, E1> fir(E1&& e1, const univector<T, Tag>& taps)
{
    return expression_fir<T, expression_value_type<E1>, E1>(std::forward<E1>(e1), taps.ref());
}

/**
 * @brief Returns template expression that applies FIR filter to the input
 * @param state FIR filter state
 * @param e1 an input expression
 */
template <typename T, typename U, typename E1>
KFR_INTRINSIC expression_fir<T, U, E1, true> fir(fir_state<T, U>& state, E1&& e1)
{
    return expression_fir<T, U, E1, true>(std::forward<E1>(e1), state);
}

/**
 * @brief Returns template expression that performs moving sum on the input
 * @param e1 an input expression
 */
template <size_t sum_length, typename E1>
KFR_INTRINSIC expression_moving_sum<expression_value_type<E1>, E1, tag_dynamic_vector> moving_sum(E1&& e1)
{
    return expression_moving_sum<expression_value_type<E1>, E1, tag_dynamic_vector>(std::forward<E1>(e1),
                                                                                    sum_length);
}

/**
 * @brief Returns template expression that performs moving sum on the input
 * @param state moving sum state
 * @param e1 an input expression
 */
template <typename U, typename E1, univector_tag STag>
KFR_INTRINSIC expression_moving_sum<U, E1, STag, true> moving_sum(moving_sum_state<U, STag>& state, E1&& e1)
{
    return expression_moving_sum<U, E1, STag, true>(std::forward<E1>(e1), state);
}

/**
 * @brief Returns template expression that applies FIR filter to the input (count of coefficients must be in
 * range 2..32)
 * @param e1 an input expression
 * @param taps coefficients for the FIR filter
 */
template <typename T, size_t TapCount, typename E1>
KFR_INTRINSIC expression_short_fir<next_poweroftwo(TapCount - 1) + 1, T, expression_value_type<E1>, E1>
short_fir(E1&& e1, const univector<T, TapCount>& taps)
{
    static_assert(TapCount >= 2 && TapCount <= 33, "Use short_fir only for small FIR filters");
    return expression_short_fir<next_poweroftwo(TapCount - 1) + 1, T, expression_value_type<E1>, E1>(
        std::forward<E1>(e1), taps);
}

/**
 * @brief Returns template expression that applies FIR filter to the input (count of coefficients must be in
 * range 2..32)
 * @param state FIR filter state
 * @param e1 an input expression
 */
template <size_t TapCount, typename T, typename U, typename E1>
KFR_INTRINSIC expression_short_fir<next_poweroftwo(TapCount - 1) + 1, T, expression_value_type<E1>, E1, true>
short_fir(short_fir_state<next_poweroftwo(TapCount - 1) + 1, T, U>& state, E1&& e1)
{
    static_assert(TapCount >= 2 && TapCount <= 33, "Use short_fir only for small FIR filters");
    return expression_short_fir<next_poweroftwo(TapCount - 1) + 1, T, expression_value_type<E1>, E1, true>(
        std::forward<E1>(e1), state);
}

template <typename T, typename U = T>
class fir_filter : public filter<U>
{
public:
    fir_filter(const univector_ref<const T>& taps) : state(taps) {}

    void set_taps(const univector_ref<const T>& taps) { state = fir_state<T, U>(taps); }

    /// Reset internal filter state
    void reset() final
    {
        state.delayline        = scalar(0);
        state.delayline_cursor = 0;
    }

protected:
    void process_buffer(U* dest, const U* src, size_t size) final
    {
        make_univector(dest, size) = fir(state, make_univector(src, size));
    }
    void process_expression(U* dest, const expression_handle<U, 1>& src, size_t size) final
    {
        make_univector(dest, size) = fir(state, src);
    }

private:
    fir_state<T, U> state;
};

template <typename T, typename U = T>
using filter_fir = fir_filter<T, U>;

} // namespace CMT_ARCH_NAME

CMT_MULTI_PROTO(template <typename U, typename T>
                filter<U>* make_fir_filter(const univector_ref<const T>& taps);)

#ifdef CMT_MULTI
template <typename U, typename T>
KFR_FUNCTION filter<U>* make_fir_filter(cpu_t cpu, const univector_ref<const T>& taps)
{
    CMT_MULTI_PROTO_GATE(make_fir_filter<U>(taps))
}
#endif
} // namespace kfr

CMT_PRAGMA_MSVC(warning(pop))
