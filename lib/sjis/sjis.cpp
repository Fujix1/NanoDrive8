#include "sjis.h"

// SJIS -> utf-8

static bool x68kExtendedToUnicode(u16_t code, u16_t& unicode) {
  // EB9F-EC9E is the JIS user-defined gaiji area. These are not standard
  // X68000 assignments, but observed MDX titles use them for these numerals.
  if (code == 0xEC4C) {
    unicode = 0x0020;  // Observed after TRIGON title; display as blank.
    return true;
  }
  if (code == 0xEC71) {
    unicode = 0x2163;  // Roman numeral four
    return true;
  }
  if (code == 0xEC7B) {
    unicode = 0x2161;  // Roman numeral two
    return true;
  }

  static const u16_t superscriptDigits[10] = {
      0x2070, 0x00B9, 0x00B2, 0x00B3, 0x2074, 0x2075, 0x2076, 0x2077, 0x2078, 0x2079,
  };
  if ((code >= 0xF030 && code <= 0xF039) || (code >= 0xF130 && code <= 0xF139)) {
    unicode = superscriptDigits[code & 0x000f];
    return true;
  }
  if ((code >= 0xF041 && code <= 0xF05A) || (code >= 0xF141 && code <= 0xF15A)) {
    unicode = 0xE020 + ((code & 0x00ff) - 0x41);
    return true;
  }
  if ((code >= 0xF230 && code <= 0xF239) || (code >= 0xF330 && code <= 0xF339)) {
    unicode = 0x2080 + (code & 0x000f);
    return true;
  }
  if ((code >= 0xF241 && code <= 0xF25A) || (code >= 0xF341 && code <= 0xF35A)) {
    unicode = 0xE000 + ((code & 0x00ff) - 0x41);
    return true;
  }
  return false;
}

String sjisToUtf8(const std::vector<u8_t>& sjis_data) {
  String result;
  result.reserve(sjis_data.size() * 2);

  size_t i = 0;
  while (i < sjis_data.size()) {
    u8_t b1 = sjis_data[i++];

    u16_t code;
    u16_t unicode;

    // ASCII (0x00-0x7F)
    if (b1 < 0x80) {
      result += (char)b1;
      continue;
    }

    // 半角カナ (0xA1-0xDF) → 1バイト
    if (b1 >= 0xA1 && b1 <= 0xDF) {
      code = b1;
      unicode = pgm_read_word_near(cp932_to_unicode + code);
    } else {
      // 2バイト文字
      if (i >= sjis_data.size()) {
        result += '?';
        break;
      }
      u8_t b2 = sjis_data[i++];
      code = (b1 << 8) | b2;
      if (!x68kExtendedToUnicode(code, unicode)) {
        unicode = pgm_read_word_near(cp932_to_unicode + code);
      }
    }

    if (unicode == 0xFFFD) {
      result += '?';  // 不明文字
      continue;
    }

    // Unicode → UTF-8 変換
    if (unicode < 0x80) {
      result += (char)unicode;
    } else if (unicode < 0x800) {
      result += (char)(0xC0 | (unicode >> 6));
      result += (char)(0x80 | (unicode & 0x3F));
    } else {
      result += (char)(0xE0 | (unicode >> 12));
      result += (char)(0x80 | ((unicode >> 6) & 0x3F));
      result += (char)(0x80 | (unicode & 0x3F));
    }
  }
  return result;
}
