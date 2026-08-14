#ifndef VERSION_H
#define VERSION_H

#include <Arduino.h>

// =============================================================================
// FIRMWARE VERSION DEFINITIONS, e.g. 0.3, 0.3.6, 2.1b
// =============================================================================
#define FW_VERSION_MAJOR    0     // 0-2147
#define FW_VERSION_MINOR    3     // 0-99
#define FW_VERSION_PATCH    5     // 0-99
#define FW_VERSION_SUFFIX   NONE  // NONE, a, b, ... (w/o suffix, alpha, beta, ...)

// =============================================================================
// BUILD DATE AND TIME
// =============================================================================
#define FW_BUILD_DATE       __DATE__     // Format: "Aug 14 2026"
#define FW_BUILD_TIME       __TIME__     // Format: "08:38:23"
#define FW_BUILD_DATETIME   __DATE__ " " __TIME__

// =============================================================================
// PREPROCESSOR HELPERS (convert to character and string)
// =============================================================================
#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

// Internal mapping for character/string concatenation
#define CHARIFY_NONE '\0'
#define CHARIFY_a 'a'
#define CHARIFY_b 'b'
#define CHARIFY_c 'c'
#define CHARIFY_d 'd'
#define CHARIFY_dev 'd'
#define CHARIFY_rc1 'r'

#define STR_NONE ""
#define STR_a "a"
#define STR_b "b"
#define STR_c "c"
#define STR_d "d"
#define STR_dev "dev"
#define STR_rc1 "rc1"

// Two-level macro expansion (forces expansion of FW_VERSION_SUFFIX -> b)
#define CHARIFY_CONCAT(x) CHARIFY_##x
#define CHARIFY(x) CHARIFY_CONCAT(x)

#define STR_CONCAT(x) STR_##x
#define STR(x) STR_CONCAT(x)

// Automatic generation of suffix character and string
#define FW_SUFFIX_CHAR  CHARIFY(FW_VERSION_SUFFIX)
#define FW_SUFFIX_STR   STR(FW_VERSION_SUFFIX)

// =============================================================================
// BUILD VERSION STRING (TEXT)
// =============================================================================
#if FW_VERSION_PATCH > 0 || (FW_SUFFIX_CHAR != '\0')
  #define FW_VERSION_STR TOSTRING(FW_VERSION_MAJOR) "." TOSTRING(FW_VERSION_MINOR) "." TOSTRING(FW_VERSION_PATCH) FW_SUFFIX_STR
#elif FW_VERSION_MINOR > 0
  #define FW_VERSION_STR TOSTRING(FW_VERSION_MAJOR) "." TOSTRING(FW_VERSION_MINOR) FW_SUFFIX_STR
#else
  #define FW_VERSION_STR TOSTRING(FW_VERSION_MAJOR) FW_SUFFIX_STR
#endif

// =============================================================================
// NUMERIC VERSION CODE (FOR CONDITIONALS IN CODE)
// =============================================================================
#define FW_SUFFIX_VAL ( \
  (FW_SUFFIX_CHAR == '\0') ? 99 : \
  (FW_SUFFIX_CHAR >= 'a' && FW_SUFFIX_CHAR <= 'z') ? (FW_SUFFIX_CHAR - 'a' + 1) : \
  (FW_SUFFIX_CHAR >= 'A' && FW_SUFFIX_CHAR <= 'Z') ? (FW_SUFFIX_CHAR - 'A' + 1) : 0 \
)

#define FW_VERSION_CODE ((FW_VERSION_MAJOR * 1000000L) + (FW_VERSION_MINOR * 10000L) + (FW_VERSION_PATCH * 100L) + FW_SUFFIX_VAL)

#define FW_VERSION_NAME "VTomRadio-MaSo-BT"

#endif // VERSION_H
