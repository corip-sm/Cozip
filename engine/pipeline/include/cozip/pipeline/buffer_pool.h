#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

#include "cozip/pipeline/pipeline_types.h"

namespace cozip::pipeline
{
class BufferPool
{
public:
    BufferPool(std::size_t block_size_bytes, std::size_t reserve_blocks);

    [[nodiscard]] BufferBlockPtr Acquire();
    void Release(BufferBlockPtr block);

    [[nodiscard]] std::size_t BlockSizeBytes() const noexcept;

private:
    std::size_t block_size_bytes_ = 0;
    std::vector<BufferBlockPtr> free_list_;
    mutable std::mutex mutex_;
};
}
