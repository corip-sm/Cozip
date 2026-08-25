#pragma once

#include "zip_archive_internal.h"

namespace cozip::format_zip
{
ZipOperationResult ExecuteChunkedCpuEntry(std::ostream& output,
                                          ZipEntrySource& entry,
                                          storage::IRandomAccessReader& reader,
                                          const pipeline::PipelineOptions& pipeline_options,
                                          std::size_t chunk_size_bytes,
                                          const core::ExecutionContext& context);

#if defined(COZIP_ENABLE_TEST_HOOKS)
void SetChunkCompressionFailureForTesting(std::size_t chunk_index) noexcept;
void ClearChunkCompressionFailureForTesting() noexcept;
#endif
}
