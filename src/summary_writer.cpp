#include "summary_writer.h"

// Same ten columns as the original writer. A separate aggregate writer avoids
// enlarging every queued CaptureRow just to carry this longer, numeric-only extra.
void csvWriteSummary(Print& out, const MinuteSummary& s, uint32_t now) {
  out.printf("%lu,,SUMMARY,,,,\"minute\",,,\"v=1 start_ms=%lu dur_ms=%lu partial=%u",
             static_cast<unsigned long>(now),static_cast<unsigned long>(s.startMs),
             static_cast<unsigned long>(s.durationMs),s.partial);
  const char* heard[]={"wifi_heard","data_heard","ble_heard"};
  const char* types[]={"probe","beacon","data","ble"};
  for (unsigned i=0;i<3;i++) out.printf(" %s=%lu",heard[i],static_cast<unsigned long>(s.heard[i]));
  for (unsigned i=0;i<4;i++) out.printf(" %s_rows=%lu %s_unique=%lu",types[i],
      static_cast<unsigned long>(s.rows[i]),types[i],static_cast<unsigned long>(s.unique[i]));
  out.printf(" apple_rows=%lu apple_unique=%lu apple_collapsed=%lu tracker_rows=%lu tracker_unique=%lu",
      static_cast<unsigned long>(s.appleRows),static_cast<unsigned long>(s.appleUnique),
      static_cast<unsigned long>(s.appleCollapsed),static_cast<unsigned long>(s.trackerRows),
      static_cast<unsigned long>(s.trackerUnique));
  for (unsigned i=0;i<WL_CAT_COUNT;i++) out.printf(" det_%s=%lu",wlCategoryName(i),
                                               static_cast<unsigned long>(s.detections[i]));
  out.printf(" dropped=%lu free_bytes=%lu unique_capped=%u apple_cache_capped=%u\"\n",
      static_cast<unsigned long>(s.dropped),static_cast<unsigned long>(s.freeBytes),
      s.uniqueCapped,s.appleCacheCapped);
}
