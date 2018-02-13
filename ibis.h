void IBIS_init() {
  Serial.begin(1200, SERIAL_7E2);
}

void IBIS_processSpecialCharacters(String* telegram) {
  telegram->replace("ä", "{");
  telegram->replace("ö", "|");
  telegram->replace("ü", "}");
  telegram->replace("ß", "~");
  telegram->replace("Ä", "[");
  telegram->replace("Ö", "\\");
  telegram->replace("Ü", "]");
}

String IBIS_vdvHex(byte value) {
  String vdvHexCharacters = "0123456789:;<=>?";
  String vdvHexValue;
  byte highNibble = value >> 4;
  byte lowNibble = value & 15;
  if (highNibble > 0) {
    vdvHexValue += vdvHexCharacters.charAt(highNibble);
  }
  vdvHexValue += vdvHexCharacters.charAt(lowNibble);
  return vdvHexValue;
}

String IBIS_wrapTelegram(String telegram) {
  telegram += '\x0d';
  unsigned char checksum = 0x7F;
  for (int i = 0; i < telegram.length(); i++) {
    checksum ^= (unsigned char)telegram[i];
  }
  // Get ready for a retarded fucking Arduino workaround
  telegram += " ";
  telegram.setCharAt(telegram.length() - 1, checksum); // seriously fuck that
  return telegram;
}

void IBIS_sendTelegram(String telegram) {
  IBIS_processSpecialCharacters(&telegram);
  telegram = IBIS_wrapTelegram(telegram);
  Serial.print(telegram);
}

void IBIS_DS003a(String text) {
  String telegram;
  byte numBlocks = ceil(text.length() / 16.0);
  telegram = "zA";
  telegram += IBIS_vdvHex(numBlocks);
  telegram += text;
  byte remainder = text.length() % 16;
  if (remainder > 0) {
    for (byte i = 16; i > remainder; i--) {
      telegram += " ";
    }
  }
  IBIS_sendTelegram(telegram);
}

void IBIS_DS003c(String text) {
  String telegram;
  byte numBlocks = ceil(text.length() / 4.0);
  telegram = "zI";
  telegram += IBIS_vdvHex(numBlocks);
  telegram += text;
  byte remainder = text.length() % 4;
  if (remainder > 0) {
    for (byte i = 4; i > remainder; i--) {
      telegram += " ";
    }
  }
  IBIS_sendTelegram(telegram);
}

void IBIS_DS009(String text) {
  String telegram = "v" + text;
  IBIS_sendTelegram(telegram);
}

void IBIS_GSP(byte address, String line1, String line2) {
  String telegram;
  String lines;
  lines += line1;
  if (line2.length() > 0) {
    lines += "\x0a";
  }
  lines += line2;
  lines += "\x0a\x0a";
  byte numBlocks = ceil(lines.length() / 16.0);
  byte remainder = lines.length() % 16;
  if (remainder > 0) {
    for (byte i = 16; i > remainder; i--) {
      lines += " ";
    }
  }
  telegram = "aA";
  telegram += IBIS_vdvHex(address);
  telegram += IBIS_vdvHex(numBlocks);
  telegram += lines;
  IBIS_sendTelegram(telegram);
}

