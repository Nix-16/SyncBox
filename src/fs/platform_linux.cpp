#include <syncbox/fs/types.hpp>

namespace syncbox::fs {

PathSupport PathSupport::platform() {
  PathSupport support;
  support.case_sensitive = true;
  support.normalizes_unicode = false;
  support.max_path_bytes = 4096;
  support.max_segment_bytes = 255;
  return support;
}

}  // namespace syncbox::fs
