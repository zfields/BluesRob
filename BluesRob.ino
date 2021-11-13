#include <NesRob.h>
#include <Notecard.h>

// Include architecture specific headers
#ifdef ARDUINO_ARCH_AVR
#include <SoftwareReset.h>
#elif ARDUINO_ARCH_ESP32
#include <WiFi.h>
#endif

#define productUID "com.blues.zfields:showcase"

#define L D10
#define S D11

#define IDLE_DELAY_MS 100
#define MAX_COMMAND_RETRIES 3
#define ROB_COMMAND_DELAY_MS 5

#ifndef NDEBUG
#define serialDebug Serial
#endif

// Interrupt variables
static volatile size_t last_command_ms = 0;
static volatile bool notehub_request = false;
static volatile bool soft_reset = false;

// Globals
bool retry_command = false;
char command_guid[37] = {0};
char signal_buffer[60] = {0};
static NesRob::Command cmd = NesRob::Command::LED_ENABLE;

NesRob rob(S, NesRob::CommandTarget::MAIN_CPU);
Notecard notecard;

#ifdef ARDUINO_ARCH_ESP32
void IRAM_ATTR ISR_notehubRequest() {
#else
void ISR_notehubRequest() {
#endif
  notehub_request = true;
}

#ifdef ARDUINO_ARCH_ESP32
void IRAM_ATTR ISR_processingComplete (void) {
#else
void ISR_processingComplete (void) {
#endif
  last_command_ms = 0;
}

#ifdef ARDUINO_ARCH_ESP32
void IRAM_ATTR ISR_softReset() {
#else
void ISR_softReset() {
#endif
  soft_reset = true;
}

int armAttnInterrupt (void) {
  int result;
  J * req;

  if (!(req = notecard.newCommand("card.attn"))) {
    result = -1;
  } else if (!(JAddStringToObject(req, "mode", "rearm,signal"))) {
    result = -2;
  } else if (!notecard.sendRequest(req)) {
    result = -3;
  } else {
    result = 0;
  }
  if (0 > result) {
    JDelete(req); // Recursively deletes nested objects and strings
  }

  return result;
}

int configureNotecard (void) {
  int result;
  J *req;

  if (!(req = notecard.newRequest("hub.set"))) {
    result = -1;
  } else if (!(JAddStringToObject(req, "mode", "continuous"))) {
    result = -2;
  } else if (!(JAddStringToObject(req, "product", productUID))) {
    result = -3;
  } else if (!(JAddBoolToObject(req, "sync", true))) {
    result = -4;
  } else if (!notecard.sendRequest(req)) {
    result = -5;
  } else {
    result = 0;
  }
  if (0 > result) {
    JDelete(req); // Recursively deletes nested objects and strings
  }

  return result;
}

J * captureSignal (void) {
  J * result;

  // Read in signal
  const size_t signal_length = Serial1.readBytesUntil('\n', signal_buffer, (sizeof(signal_buffer)-1));
  if (59 != signal_length) {
    result = JParse("{\"err\":\"Unexpected signal length!\"}\r\n");
  } else {
    result = JParse(signal_buffer);
  }

  return result;
}

int enableSignalRequest (void) {
  int result;
  J * req;

  // Configure UART
  Serial1.begin(115200);
  Serial1.setTimeout(10);

  // Configure Notecard
  if (!(req = notecard.newRequest("hub.signal"))) {
    result = -1;
  } else if (!(JAddBoolToObject(req, "on", true))) {
    result = -2;
  } else if (!notecard.sendRequest(req)) {
    result = -3;
  } else {
    result = 0;
  }
  if (0 > result) {
    JDelete(req); // Recursively deletes nested objects and strings
  }

  return result;
}

void emptyNotecardQueue (void) {
  for (bool empty = false ; !empty ;) {
    if (J * signal = captureSignal()) {
      if (notecard.responseError(signal)) {
        notehub_request = false;
        empty = true;
      } else {
#ifndef NDEBUG
        notecard.logDebugf("Deleted message with contents:\n\t> %s, JConvertToJSONString(rsp)");
#endif
      }
      notecard.deleteResponse(signal);
    }
  }

  return;
}

int processRequest (NesRob::Command cmd_) {
  int result;

  //TODO: Implement await modem
  for (bool modem_active = true ; modem_active ;) {
    ::delay(75);
    modem_active = false;
  }

  // Signal Command
  if (rob.sendCommand(cmd_)) {
    result = 1;
  } else {
    last_command_ms = ::millis();
    result = 0;
  }

  return result;
}

int reportProcessedCommand (void) {
  int result;
  J *req;
  J *body;

  if (!(req = notecard.newCommand("hub.signal"))) {
    result = -1;
  } else if (!(body = JAddObjectToObject(req, "body"))) {
    result = -2;
  } else if (!(JAddStringToObject(body, "guid", command_guid))) {
    result = -3;
  } else if (!notecard.sendRequest(req)) {
    result = -4;
  } else {
    result = 0;
  }
  if (0 > result) {
    JDelete(req); // Recursively deletes nested objects and strings
  }

  return result;
}

void systemReset (void) {
#ifndef NDEBUG
  notecard.logDebug("INFO: Device reset requested.\n");
#endif
#ifdef ARDUINO_ARCH_AVR
  softwareReset::standard();
#elif ARDUINO_ARCH_ESP32
  ESP.restart();
#elif ARDUINO_ARCH_SAM
  ::rstc_start_software_reset(RSTC);
#elif ARDUINO_ARCH_SAMD
  ::NVIC_SystemReset();
#else
  void(*restart)(void) = nullptr;
  restart();
#endif
}

  //***********
 //** SETUP **
//***********
void setup() {
#ifdef ARDUINO_ARCH_ESP32
  // Disable radios to improve power profile
  WiFi.mode(WIFI_OFF);
  ::btStop();
#endif

  // Debug LED
  ::digitalWrite(LED_BUILTIN, LOW);
  ::pinMode(LED_BUILTIN, OUTPUT);

#ifndef NDEBUG
  // Initialize Debug Output
  serialDebug.begin(115200);
  while (!serialDebug) {
    ; // wait for serial port to connect. Needed for native USB
  }
  notecard.setDebugOutputStream(serialDebug);
#endif

  // Initialize Notecard
  notecard.begin();

  // Configure Notecard
  if (configureNotecard()) {
#ifndef NDEBUG
    notecard.logDebug("FATAL: Failed to configure Notecard!\n");
#endif
    systemReset();

  // Enable Signal Requests
  } else if (enableSignalRequest()) {
#ifndef NDEBUG
    notecard.logDebug("FATAL: Failed to enable signal requests!\n");
#endif
    systemReset();

  // Arm ATTN Interrupt
  } else if (armAttnInterrupt()) {
#ifndef NDEBUG
    notecard.logDebug("FATAL: Failed to rearm ATTN interrupt!\n");
#endif
    systemReset();

  } else {
    // Attach R.O.B. Interrupt
    ::pinMode(L, INPUT);
    ::attachInterrupt(digitalPinToInterrupt(L), ISR_processingComplete, RISING);

    // Empty Notecard Queue
    emptyNotecardQueue();

    // Put R.O.B. into known state
    ::digitalWrite(LED_BUILTIN, HIGH);
    for (last_command_ms = millis() ; last_command_ms ;) {
        if (rob.sendCommand(NesRob::Command::LED_ENABLE)) {
        ::digitalWrite(LED_BUILTIN, LOW);
        ::delay(IDLE_DELAY_MS);
        ::digitalWrite(LED_BUILTIN, HIGH);
        }
    }
    ::digitalWrite(LED_BUILTIN, LOW);

    // Attach Button Interrupt
    ::pinMode(B0, INPUT_PULLUP);
    ::attachInterrupt(digitalPinToInterrupt(B0), ISR_softReset, RISING);

    // Attach Notecard Interrupt
    ::pinMode(D6, INPUT);
    ::attachInterrupt(digitalPinToInterrupt(D6), ISR_notehubRequest, RISING);
  }
}

  //**********
 //** LOOP **
//**********
void loop() {
  // Ready to process?
  if (last_command_ms) {
    static size_t retry_count = 0;

    bool rob_working = !::digitalRead(L);
    bool times_up = ((::millis() - ROB_COMMAND_DELAY_MS) > last_command_ms);

    if (rob_working) {
      // R.O.B. is executing a command
      retry_count = 0;

      // Report processed command GUID to Notehub
      if (*command_guid) {
        if (reportProcessedCommand()) {
#ifndef NDEBUG
          notecard.logDebug("ERROR: Failed to send Signal!\n");
#endif
        }

        *command_guid = '\0';
      }
    }

    if (!rob_working && times_up) {
      // Failed to issue command
      if (MAX_COMMAND_RETRIES > retry_count) {
        ++retry_count;
        retry_command = true;
      }
      last_command_ms = 0;
    } else {
      // Await idle delay
      ::delay(IDLE_DELAY_MS);
      return;
    }
  }

  // Button pressed?
  if (soft_reset) {
    // Clear flags
    retry_command = false;

    // Empty Queue
    emptyNotecardQueue();

    // Rearm ATTN Interrupt
    if (armAttnInterrupt()) {
#ifndef NDEBUG
      notecard.logDebug("ERROR: Failed to rearm ATTN interrupt!\n");
#endif
    }

    // Load RECALIBRATE Command
    *command_guid = '\0';
    cmd = NesRob::Command::RECALIBRATE;

  // Reprocess dropped command
  } else if (retry_command) {

  // Process queue
  } else if (notehub_request) {
    notehub_request = false;
    if (J * signal = captureSignal()) {
      if (notecard.responseError(signal)) {
#ifndef NDEBUG
        notecard.logDebug("ERROR: Unrecognized signal format!\n");
#endif
      } else {
        // Process signal contents
        if (JHasObjectItem(signal,"cmd")) {
          J * cmd_obj = JGetObjectItem(signal, "cmd");

          if (JIsNumber(cmd_obj)) {
            cmd = static_cast<NesRob::Command>(JNumberValue(cmd_obj));
#ifndef NDEBUG
            notecard.logDebugf("Received command: 0x%x\n", cmd);
#endif
          } else {
#ifndef NDEBUG
            notecard.logDebugf("ERROR: Command must be an integer type! Type provided: %s\n", JType(cmd_obj));
#endif
          }
        } else {
#ifndef NDEBUG
          notecard.logDebug("ERROR: Missing `cmd` field!\n");
#endif
        }

        if (JHasObjectItem(signal,"guid")) {
          J * guid_obj = JGetObjectItem(signal, "guid");
          if (JIsString(guid_obj)) {
            ::strcpy(command_guid, JGetStringValue(guid_obj));
#ifndef NDEBUG
            notecard.logDebugf("Processing command guid: %s\n", command_guid);
#endif
          } else {
#ifndef NDEBUG
            notecard.logDebugf("ERROR: Note `guid` must be an GUID (string) type! Type provided: %s\n", JType(guid_obj));
#endif
          }
        } else {
#ifndef NDEBUG
          notecard.logDebug("ERROR: Missing `guid` field!\n");
#endif
        }
      }
      // Delete response
      notecard.deleteResponse(signal);
    } else {
#ifndef NDEBUG
      notecard.logDebug("ERROR: Notecard communication error!\n");
#endif
    }

    // Rearm ATTN Interrupt
    if (armAttnInterrupt()) {
#ifndef NDEBUG
      notecard.logDebug("ERROR: Failed to rearm ATTN interrupt!\n");
#endif
    }
  } else {
    // Await idle delay
    ::delay(IDLE_DELAY_MS);
    return;
  }

  // Issue command to R.O.B.
  if (processRequest(cmd)) {
#ifndef NDEBUG
    notecard.logDebug("ERROR: Failed to command R.O.B.!\n");
#endif
    return;
  }

  if (soft_reset) {
    soft_reset = false;
  } else {
    if (retry_command) {
      retry_command = false;
    }

    // Check Queue
    notehub_request = Serial1.available();
#ifndef NDEBUG
    if (notehub_request) {
      notecard.logDebug("Discovered additional Note(s).\n");
    } else {
      notecard.logDebug("All Notes processed.\n");
    }
#endif
  }
}
