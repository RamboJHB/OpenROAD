#pragma once

#include <tbb/parallel_for.h>
#include <tbb/task_arena.h>
#include <util/assert.h>

#include <tuple>
#include <utility>

namespace dpl2 {

class DrcUtil {

public:
    // mt
    static tbb::task_arena* getArena()
    {
        return _arena;
    }

    static void setArena(tbb::task_arena* arena)
    {
        _arena = arena;
    }

    template<typename Func, typename... Args>
    static void parallelFor(unsigned long task, Func&& runJob, Args&&... args)
    {
        if (task < 128) {
            _arena->execute([&]() {
                tbb::parallel_for(0UL, task, [&](unsigned long i) {
                    auto locals = std::forward_as_tuple(args.local()...);
                    std::apply([&](auto&... localArgs) {
                        runJob(i, localArgs...);
                    }, locals);
                });
            });
        } else {
            auto batchSize = task / (_arena->max_concurrency() * 1024) + 4;
            _arena->execute([&]() {
                tbb::parallel_for(tbb::blocked_range<unsigned long>(0, task,
                    batchSize), [&](const tbb::blocked_range<unsigned long>
                    &batchRange) {
                    auto locals = std::forward_as_tuple(args.local()...);
                    for (unsigned long i = batchRange.begin();
                        i < batchRange.end(); ++i) {
                        std::apply([&](auto&... localArgs) {
                            runJob(i, localArgs...);
                        }, locals);
                    }
                });
            });
        }
    }

    template<typename Func, typename... Args>
    static void parallelFor(unsigned long start, unsigned long end,
        Func&& runJob, Args&&... args)
    {
        uvAssert(start <= end);
        if (end - start < 128) {
            _arena->execute([&]() {
                tbb::parallel_for(start, end, [&](unsigned long i) {
                    auto locals = std::forward_as_tuple(args.local()...);
                    std::apply([&](auto&... localArgs) {
                        runJob(i, localArgs...);
                    }, locals);
                });
            });
        } else {
            auto batchSize = (end - start) / (_arena->max_concurrency() * 1024)
                + 4;
            _arena->execute([&]() {
                tbb::parallel_for(tbb::blocked_range<unsigned long>(start,
                    end, batchSize),
                    [&](const tbb::blocked_range<unsigned long> &batchRange) {
                    auto locals = std::forward_as_tuple(args.local()...);
                    for (unsigned long i = batchRange.begin();
                        i < batchRange.end(); ++i) {
                        std::apply([&](auto&... localArgs) {
                            runJob(i, localArgs...);
                        }, locals);
                    }
                });
            });
        }
    }

protected:
    static tbb::task_arena* _arena;

};

} // namespace dpl2