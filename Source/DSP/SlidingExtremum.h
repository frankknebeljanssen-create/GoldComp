#pragma once

#include <vector>
#include <cstddef>

/**
 * SlidingMinimum — minimum of the last N pushed values, in O(1) amortised.
 *
 * Both lookahead stages need "what is the lowest gain coming up in the next N
 * samples". Scanning the window costs N compares and N integer divisions per
 * sample, which is the single most expensive thing in the plugin once the
 * window grows past a few dozen samples. A monotonic deque replaces that with
 * roughly one compare per sample: values that can never be the minimum again
 * (because a smaller one arrived later) are discarded on the way in, so the
 * front is always the answer.
 *
 * Allocation happens only in prepare().
 */
class SlidingMinimum
{
public:
    void prepare (int windowSize)
    {
        window = windowSize < 1 ? 1 : windowSize;
        capacity = window + 1;
        values.assign ((size_t) capacity, 0.0f);
        indices.assign ((size_t) capacity, 0LL);
        reset();
    }

    void reset (float fillValue = 0.0f)
    {
        head = tail = 0;
        count = 0;
        position = 0;
        // Seed with one entry so the window behaves as if it were full of
        // fillValue — otherwise the first N samples would see a shorter window.
        values[0] = fillValue;
        indices[0] = 0;
        tail = 1;
        count = 1;
        position = 1;
    }

    /** Pushes the newest value and returns the minimum across the window. */
    float pushAndGet (float v)
    {
        // Drop entries that are no longer smaller than the newcomer: once a
        // smaller value arrives behind them they can never win again.
        while (count > 0)
        {
            int last = (tail - 1 + capacity) % capacity;
            if (values[(size_t) last] >= v) { tail = last; --count; }
            else break;
        }
        values[(size_t) tail] = v;
        indices[(size_t) tail] = position;
        tail = (tail + 1) % capacity;
        ++count;

        // Retire the front once it falls outside the window.
        while (count > 0 && indices[(size_t) head] <= position - window)
        {
            head = (head + 1) % capacity;
            --count;
        }

        ++position;
        return values[(size_t) head];
    }

private:
    std::vector<float>     values;
    std::vector<long long> indices;   // absolute sample positions, so 64-bit
    int window = 1, capacity = 2;
    int head = 0, tail = 0, count = 0;
    long long position = 0;
};
