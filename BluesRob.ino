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

#define DEBUG 1
#define IDLE_DELAY_MS 100
#define MAX_COMMAND_RETRIES 3
#define ROB_COMMAND_DELAY_MS 5

#if DEBUG
#define serialDebug Serial
#endif

// Interrupt variables
static volatile int last_command_ms = 0;
static volatile bool notehub_request = false;
static volatile bool soft_reset = false;

// Globals
bool retry_command = false;
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

int armAttnInterrupt (const char * queue_) {
  int result;
  J * req;
  J * files;

  if (!(req = NoteNewRequest("card.attn"))) {
    result = -1;
  } else if (!(JAddStringToObject(req, "mode", "rearm,files"))) {
    result = -2;
  } else if (!(files = JAddArrayToObject(req, "files"))) {
    result = -3;
  } else {
    JAddItemToArray(files, JCreateString(queue_));
    if (!notecard.sendRequest(req)) {
      result = 1;
    } else {
      result = 0;
    }
  }

  if (result < 0) {
    notecard.logDebug("FATAL: Unable to allocate request!\n");
    systemReset();
  }

  return result;
}

J * dequeueCommand (bool pop_) {
  int result;
  J * req;
  J * rsp;

  if (!(req = NoteNewRequest("note.get"))) {
    result = -1;
  } else if (!(JAddStringToObject(req, "file", "rob.qi"))) {
    result = -2;
  } else if (!(JAddBoolToObject(req, "delete", pop_))) {
    result = -3;
  } else {
    // Get Note
    rsp = notecard.requestAndResponse(req);
    result = 0;
  }

  if (result < 0) {
    notecard.logDebug("FATAL: Unable to allocate request!\n");
    rsp = nullptr;
    systemReset();
  }

  return rsp;
}

int emptyNotecardQueue (void) {
  int result;

  for (bool empty = false ; !empty ;) {
    if (J * rsp = dequeueCommand(true)) {
      if (notecard.responseError(rsp)) {
        result = 0;
        empty = true;
      } else {
        notecard.logDebugf("Deleted message with contents:\n\t> %s, JConvertToJSONString(rsp)");
      }
      notecard.deleteResponse(rsp);
    } else {
      notecard.logDebug("ERROR: Notecard communication error!\n");
      ::delay(IDLE_DELAY_MS);
    }
  }

  return result;
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

void systemReset (void) {
#if DEBUG
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
  *(0);
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

#if DEBUG
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
  if (J *req = notecard.newRequest("hub.set")) {
    JAddNumberToObject(req, "duration", 10);
    JAddStringToObject(req, "mode", "continuous");
    JAddStringToObject(req, "product", productUID);
    JAddBoolToObject(req, "sync", true);
    if (!notecard.sendRequest(req)) {
      notecard.logDebug("FATAL: Failed to configure Notecard!\n");
      systemReset();
    }
  }

  // Arm ATTN Interrupt
  if (armAttnInterrupt("rob.qi")) {
    notecard.logDebug("ERROR: Failed to rearm ATTN interrupt!\n");
  }

  // Attach Notecard Interrupt
  ::pinMode(D6, INPUT);
  ::attachInterrupt(digitalPinToInterrupt(D6), ISR_notehubRequest, RISING);

  // Attach R.O.B. Interrupt
  ::pinMode(L, INPUT);
  ::attachInterrupt(digitalPinToInterrupt(L), ISR_processingComplete, RISING);

  // Empty Notecard Queue
  if (emptyNotecardQueue()) {
    notecard.logDebug("ERROR: Unable to empty queue!\n");
  }

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
    // Empty Queue
    if (emptyNotecardQueue()) {
      notecard.logDebug("ERROR: Unable to empty queue!\n");
    }

    // Rearm ATTN Interrupt
    if (armAttnInterrupt("rob.qi")) {
      notecard.logDebug("ERROR: Failed to rearm ATTN interrupt!\n");
    }

    // Load RECALIBRATE Command
    cmd = NesRob::Command::RECALIBRATE;

  // Reprocess dropped command
  } else if (retry_command) {

  // Process queue
  } else if (notehub_request) {
    notehub_request = false;
    if (J * rsp = dequeueCommand(false)) {
      if (notecard.responseError(rsp)) {
        notecard.logDebug("ERROR: Failed to acquire Note!\n");
      } else {
        // Process Note contents
        if (JHasObjectItem(rsp,"body")) {
          J * body_obj = JGetObjectItem(rsp, "body");
          if (JHasObjectItem(body_obj,"cmd")) {
            J * cmd_obj = JGetObjectItem(body_obj, "cmd");

            if (JIsNumber(cmd_obj)) {
              cmd = static_cast<NesRob::Command>(JNumberValue(cmd_obj));
              notecard.logDebugf("Received command: 0x%x\n", cmd);
            } else {
              //notecard.logDebugf("ERROR: Command must be an integer type! Type provided: %s\n", JType(cmd_obj));
              notecard.logDebugf("ERROR: Command must be an integer type! Type provided: %d\n", cmd_obj->type);
            }
          } else {
            notecard.logDebug("ERROR: Unrecognized Note format!\n");
          }
        } else {
          notecard.logDebug("ERROR: Unrecognized Note format!\n");
        }
      }
      // Delete response
      notecard.deleteResponse(rsp);
    } else {
      notecard.logDebug("ERROR: Notecard communication error!\n");
    }

    // Rearm ATTN Interrupt
    if (armAttnInterrupt("rob.qi")) {
      notecard.logDebug("ERROR: Failed to rearm ATTN interrupt!\n");
    }
  } else {
    // Await idle delay
    ::delay(IDLE_DELAY_MS);
    return;
  }

  // Issue command to R.O.B.
  if (processRequest(cmd)) {
    notecard.logDebug("ERROR: Failed to command R.O.B.!\n");
    return;
  }

  if (soft_reset) {
    soft_reset = false;
  } else {
    if (retry_command) {
      retry_command = false;
    } else {
      // Delete Processed Note
      if (J * rsp = dequeueCommand(true)) {
        if (notecard.responseError(rsp)) {
          notecard.logDebug("ERROR: Failed to delete Note!\n");
        }
        notecard.deleteResponse(rsp);
      } else {
        notecard.logDebug("ERROR: Notecard communication error!\n");
      }
    }

    // Check Queue
    if (J * rsp = dequeueCommand(false)) {
      if (notecard.responseError(rsp)) {
        notecard.logDebug("All Notes processed.\n");
      } else {
        notehub_request = true;
        notecard.logDebug("Discovered additional Note(s).\n");
      }
      notecard.deleteResponse(rsp);
    } else {
      notecard.logDebug("ERROR: Notecard communication error!\n");
    }
  }
}
