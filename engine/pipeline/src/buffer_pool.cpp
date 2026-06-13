#include "cozip/pipeline/buffer_pool.h"

#include <utility>

namespace cozip::pipeline
{
namespace
{
BufferBlockPtr AllocateBlock(std::size_t block_size_bytes)
{
    auto block = std::make_shared<BufferBlock>();
    block->bytes.resize(block_size_bytes);
    return block;
}
}

BufferPool::BufferPool(std::size_t block_size_bytes, std::size_t reserve_blocks)
    : block_size_bytes_(block_size_bytes)
{
    free_list_.reserve(reserve_blocks);
    for (std::size_t index = 0; index < reserve_blocks; ++index)
    {
        free_list_.push_back(AllocateBlock(block_size_bytes_));
    }
}

BufferBlockPtr BufferPool::Acquire()
{
    std::lock_guard lock(mutex_);
    if (free_list_.empty())
    {
        return AllocateBlock(block_size_bytes_);
    }

    auto block = std::move(free_list_.back());
    free_list_.pop_back();
    return block;
}

void BufferPool::Release(BufferBlockPtr block)
{
    if (!block)
    {
        return;
    }

    if (block->bytes.size() != block_size_bytes_)
    {
        block->bytes.resize(block_size_bytes_);
    }

    std::lock_guard lock(mutex_);
    free_list_.push_back(std::move(block));
}

std::size_t BufferPool::BlockSizeBytes() const noexcept
{
    return block_size_bytes_;
}
}
