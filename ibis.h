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

void IBIS_DS009(String text) {
  String telegram = "v" + text;
  IBIS_sendTelegram(telegram);
}

