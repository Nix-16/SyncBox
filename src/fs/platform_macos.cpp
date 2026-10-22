#include <syncbox/fs/types.hpp>

namespace syncbox::fs {

PathSupport PathSupport::platform() {
  PathSupport support;
  // Default APFS/HFS+ volumes are case-insensitive; HFS+ also normalizes
  // Unicode to a decomposed form.
  support.case_sensitive = false;
  support.normalizes_unicode = true;
  support.max_path_bytes = 1024;
  support.max_segment_bytes = 255;
  return support;
}

}  // namespace syncbox::fs
