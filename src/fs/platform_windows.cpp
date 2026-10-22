#include <syncbox/fs/types.hpp>

namespace syncbox::fs {

PathSupport PathSupport::platform() {
  PathSupport support;
  support.case_sensitive = false;
  support.normalizes_unicode = false;
  support.max_path_bytes = 260;  // conservative without long-path opt-in
  support.max_segment_bytes = 255;
  return support;
}

}  // namespace syncbox::fs
