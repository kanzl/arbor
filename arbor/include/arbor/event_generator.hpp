#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <random>
#include <type_traits>

#include <arbor/assert.hpp>
#include <arbor/common_types.hpp>
#include <arbor/generic_event.hpp>
#include <arbor/spike_event.hpp>
#include <arbor/schedule.hpp>

namespace arb {

// An `event_generator` generates a sequence of events to be delivered to a cell.
// The sequence of events is always in ascending order, i.e. each event will be
// greater than the event that proceeded it, where events are ordered by:
//  - delivery time;
//  - then target id for events with the same delivery time;
//  - then weight for events with the same delivery time and target.
//
// An `event_generator` supports three operations:
//
// `void event_generator::reset()`
//
//     Reset generator state.
//
// `event_seq event_generator::events(time_type to, time_type from)`
//
//     Provide a non-owning view on to the events in the time interval
//     [to, from).
//
// `void resolve_label(resolution_function)`
//
//     event_generators are constructed on cable_local_label_types comprising
//     a label and a selection policy. These labels need to be resolved to a
//     specific cell_lid_type. This is done using a resolution_function.
//
// The `event_seq` type is a pair of `spike_event` pointers that
// provide a view onto an internally-maintained contiguous sequence
// of generated spike event objects. This view is valid only for
// the lifetime of the generator, and is invalidated upon a call
// to `reset` or another call to `events`.
//
// Calls to the `events` method must be monotonic in time: without an
// intervening call to `reset`, two successive calls `events(t0, t1)`
// and `events(t2, t3)` to the same event generator must satisfy
// 0 ≤ t0 ≤ t1 ≤ t2 ≤ t3.
//
// `event_generator` objects have value semantics, and use type erasure
// to wrap implementation details. An `event_generator` can be constructed
// from an object of an implementation class Impl that is copy-constructable
// and otherwise provides `reset` and `events` methods following the
// API described above.
//
// Some pre-defined event generators are included:
//  - `empty_generator`: produces no events
//  - `schedule_generator`: produces events according to a time schedule.
//    A target is selected using a label resolution function for every generated
//    event.
//  - `explicit_generator`: is constructed from a vector of {label, time, weight}
//    objects. Explicit targets are generated from the labels using a resolution
//    function before the first call to the `events` method.

using event_seq = std::pair<const spike_event*, const spike_event*>;
using resolution_function = std::function<cell_lid_type(const cell_local_label_type&)>;

// The simplest possible generator that generates no events.
// Declared ahead of event_generator so that it can be used as the default
// generator.
struct empty_generator {
    void reset() {}
    event_seq events(time_type, time_type) {
        return {nullptr, nullptr};
    }
    void resolve_label(resolution_function) {}
};

class event_generator {
public:
    event_generator(): event_generator(empty_generator()) {}

    template <typename Impl, std::enable_if_t<!std::is_same<std::decay_t<Impl>, event_generator>::value, int> = 0>
    event_generator(Impl&& impl):
        impl_(new wrap<Impl>(std::forward<Impl>(impl)))
    {}

    event_generator(event_generator&& other) = default;
    event_generator& operator=(event_generator&& other) = default;

    event_generator(const event_generator& other):
        impl_(other.impl_->clone())
    {}

    event_generator& operator=(const event_generator& other) {
        impl_ = other.impl_->clone();
        return *this;
    }

    void reset() {
        impl_->reset();
    }

    event_seq events(time_type t0, time_type t1) {
        return impl_->events(t0, t1);
    }

    void resolve_label(resolution_function label_resolver) {
        impl_->resolve_label(std::move(label_resolver));
    }

private:
    struct interface {
        virtual void reset() = 0;
        virtual void resolve_label(resolution_function) = 0;
        virtual event_seq events(time_type, time_type) = 0;
        virtual std::unique_ptr<interface> clone() = 0;
        virtual ~interface() {}
    };

    std::unique_ptr<interface> impl_;

    template <typename Impl>
    struct wrap: interface {
        explicit wrap(const Impl& impl): wrapped(impl) {}
        explicit wrap(Impl&& impl): wrapped(std::move(impl)) {}

        event_seq events(time_type t0, time_type t1) override {
            return wrapped.events(t0, t1);
        }

        void reset() override {
            wrapped.reset();
        }

        void resolve_label(resolution_function label_resolver) override {
            wrapped.resolve_label(std::move(label_resolver));
        }

        std::unique_ptr<interface> clone() override {
            return std::unique_ptr<interface>(new wrap<Impl>(wrapped));
        }

        Impl wrapped;
    };
};

// Convenience routines for making schedule_generator:

// Generate events with a fixed target and weight according to
// a provided time schedule.

struct schedule_generator {
    schedule_generator(cell_local_label_type target, float weight, schedule sched):
        target_(std::move(target)), weight_(weight), sched_(std::move(sched))
    {}

    void resolve_label(resolution_function label_resolver) {
        label_resolver_ = std::move(label_resolver);
    }

    void reset() {
        sched_.reset();
    }

    event_seq events(time_type t0, time_type t1) {
        auto ts = sched_.events(t0, t1);

        events_.clear();
        events_.reserve(ts.second-ts.first);

        for (auto i = ts.first; i!=ts.second; ++i) {
            events_.push_back(spike_event{label_resolver_(target_), *i, weight_});
        }

        return {events_.data(), events_.data()+events_.size()};
    }

private:
    pse_vector events_;
    cell_local_label_type target_;
    resolution_function label_resolver_;
    float weight_;
    schedule sched_;
};

// Generate events at integer multiples of dt that lie between tstart and tstop.

inline event_generator regular_generator(
    cell_local_label_type target,
    float weight,
    time_type tstart,
    time_type dt,
    time_type tstop=terminal_time)
{
    return schedule_generator(std::move(target), weight, regular_schedule(tstart, dt, tstop));
}

template <typename RNG>
inline event_generator poisson_generator(
    cell_local_label_type target,
    float weight,
    time_type tstart,
    time_type rate_kHz,
    const RNG& rng,
    time_type tstop=terminal_time)
{
    return schedule_generator(std::move(target), weight, poisson_schedule(tstart, rate_kHz, rng, tstop));
}


// Generate events from a predefined sorted event sequence.

struct explicit_generator {
    struct labeled_synapse_event {
        cell_local_label_type label;
        time_type time;
        float weight;
    };

    using lse_vector = std::vector<labeled_synapse_event>;

    explicit_generator() = default;
    explicit_generator(const explicit_generator&) = default;
    explicit_generator(explicit_generator&&) = default;

    explicit_generator(const lse_vector& events):
        input_events_(events), start_index_(0) {}

    void resolve_label(resolution_function label_resolver) {
        for (const auto& e: input_events_) {
            events_.push_back({label_resolver(e.label), e.time, e.weight});
        }
        std::sort(events_.begin(), events_.end());
    }

    void reset() {
        start_index_ = 0;
    }

    event_seq events(time_type t0, time_type t1) {
        const spike_event* lb = events_.data()+start_index_;
        const spike_event* ub = events_.data()+events_.size();

        lb = std::lower_bound(lb, ub, t0, event_time_less{});
        ub = std::lower_bound(lb, ub, t1, event_time_less{});

        start_index_ = ub-events_.data();
        return {lb, ub};
    }

private:
    lse_vector input_events_;
    pse_vector events_;
    std::size_t start_index_ = 0;
};


} // namespace arb

