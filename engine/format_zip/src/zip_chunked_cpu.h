#pragma once

#include "zip_archive_internal.h"

namespace cozip::format_zip
{
ZipOperationResult ExecuteChunkedCpuEntry(std::ostream& output,
                                          ZipEntrySource& entry,
                                          const pipeline::PipelineOptions& pipeline_options,
                                          std::size_t chunk_size_bytes);
}
