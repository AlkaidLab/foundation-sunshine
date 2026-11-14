#include "alkaidlab/ds5_core/ds5_core.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  AlkDs5State state = {sizeof(state), ALK_DS5_CORE_ABI_VERSION};
  (void)alk_ds5_parse_input_report(data, size, &state);
  AlkDs5ExtendedInput extended = {sizeof(extended), ALK_DS5_CORE_ABI_VERSION};
  (void)alk_ds5_parse_input_report_ex(data, size, &extended);

  AlkDs5Feedback feedback = {sizeof(feedback), ALK_DS5_CORE_ABI_VERSION};
  (void)alk_ds5_parse_output_report_ex(data, size, &feedback);
  AlkDs5FeatureFacts feature = {sizeof(feature), ALK_DS5_CORE_ABI_VERSION};
  if (size != 0u) {
    (void)alk_ds5_parse_feature_report(data[0], data, size, &feature);
  }

  return 0;
}
