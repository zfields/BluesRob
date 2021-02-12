#include <NesRob.h>
#include <Notecard.h>

#define productUID "com.blues.zfields:showcase"

#define L D10
#define S D11

#define DEBUG 1
#define IDLE_DELAY_MS 100
#define PROCESSING_TIMEOUT_MS 7500

#if DEBUG
#define serialDebug Serial
#endif

// Interrupt variables
static volatile int last_command_ms = 0;
static volatile bool notehub_request = false;
static volatile bool soft_reset = false;

// Globals
static bool modem_active = false;
static NesRob::Command cmd = NesRob::Command::LED_ENABLE;

NesRob rob(S, NesRob::CommandTarget::MAIN_CPU);
Notecard notecard;
 
void IRAM_ATTR ISR_notehubRequest() {
  notehub_request = true;
}

void IRAM_ATTR ISR_processingComplete (void) {
  last_command_ms = 0;
}

void IRAM_ATTR ISR_softReset() {
  soft_reset = true;
}

int emptyNotecardQueue (void) {
  int result;

  for (bool empty = false ; !empty ;) {
    if (J *req = NoteNewRequest("note.get")) {
      JAddStringToObject(req, "file", "rob.qi");
      JAddBoolToObject(req, "delete", true);
  
      if (!notecard.sendRequest(req)) {
        result = 0;
        empty = true;
      }
    } else {
#if DEBUG
      notecard.logDebug("FATAL: Unable to allocate request!\n");
      while(1);
#endif
      result = 1;
      break;
    }
  }
  return result;
}

int processRequest (void) {
  int result;

  while (modem_active) {
    // check modem status
  }

  // Signal Command
  if (rob.sendCommand(cmd)) {
#if DEBUG
    notecard.logDebug("ERROR: Failed to command R.O.B.!\n");
#endif
    result = 1;
  } else {
    last_command_ms = millis();
    result = 0;
  }

  return result;
}

  //***********
 //** SETUP **
//***********
void setup() {
  // Debug LED
  digitalWrite(LED_BUILTIN, LOW);
  pinMode(LED_BUILTIN, OUTPUT);

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
    JAddStringToObject(req, "product", productUID);
    JAddStringToObject(req, "mode", "continuous");
    JAddBoolToObject(req, "sync", true);
    if (!notecard.sendRequest(req)) {
#if DEBUG
      notecard.logDebug("FATAL: Failed to configure Notecard!\n");
      while(1);
#endif
    }
  }

  // Arm ATTN Interrupt
  if (J *req = NoteNewRequest("card.attn")) {
    JAddStringToObject(req, "mode", "arm,files");
    if (J *files = JAddArrayToObject(req, "files")) {
      JAddItemToArray(files, JCreateString("rob.qi"));
      if (!notecard.sendRequest(req)) {
#if DEBUG
        notecard.logDebug("ERROR: Failed to arm ATTN interrupt!\n");
#endif
      }
    }
  }

  // Attach Notecard Interrupt
  pinMode(D6, INPUT);
  attachInterrupt(digitalPinToInterrupt(D6), ISR_notehubRequest, RISING);

  // Attach R.O.B. Interrupt
  pinMode(L, INPUT);
  attachInterrupt(digitalPinToInterrupt(L), ISR_processingComplete, RISING);

  // Empty Notecard Queue
  if (emptyNotecardQueue()) {
#if DEBUG
    notecard.logDebug("ERROR: Unable to empty queue!\n");
#endif
  }

  // Put R.O.B. into known state
  ::digitalWrite(LED_BUILTIN, HIGH);
  for (last_command_ms = millis() ; last_command_ms ;) {
    if (rob.sendCommand(NesRob::Command::LED_ENABLE)) {
      ::digitalWrite(LED_BUILTIN, LOW);
      ::delay(100);
      ::digitalWrite(LED_BUILTIN, HIGH);
    }
  }
  ::digitalWrite(LED_BUILTIN, LOW);

  // Attach Button Interrupt
  pinMode(B0, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(B0), ISR_softReset, RISING);  
}

  //**********
 //** LOOP **
//**********
void loop() {
  // Check for processing timeout
  if (last_command_ms && (millis() - PROCESSING_TIMEOUT_MS) > last_command_ms) {
    last_command_ms = 0;
  }
  
  // Ready to process?
  if (last_command_ms) {
    return;
  }

  // Button pressed?
  if (soft_reset) {
    // empty queue
    if (emptyNotecardQueue()) {
#if DEBUG
      notecard.logDebug("ERROR: Unable to empty queue!\n");
#endif
    }
    cmd = NesRob::Command::RECALIBRATE;

  // Process queue
  } else {
    // Note in queue?
    if (notehub_request) {
      notehub_request = false;
      if (J *req = NoteNewRequest("note.get")) {
        JAddStringToObject(req, "file", "rob.qi");
        JAddBoolToObject(req, "delete", false);

        // Get Note
        J* rsp = notecard.requestAndResponse(req);
        if (notecard.responseError(rsp)) {
#if DEBUG
          notecard.logDebug("ERROR: Failed to acquire Note!\n");
#endif
        }

        // Process Note contents
        if (JHasObjectItem(rsp,"body")) {
          J* body_obj = JGetObjectItem(rsp, "body");
          if (JHasObjectItem(body_obj,"cmd")) {
            J* cmd_obj = JGetObjectItem(body_obj, "cmd");

            if (JIsNumber(cmd_obj)) {
              cmd = static_cast<NesRob::Command>(cmd_obj->valuenumber);
  #if DEBUG
              notecard.logDebugf("Received command: 0x%x\n", cmd);
            } else {
              notecard.logDebugf("ERROR: Command must be an integer! %d\n", cmd_obj->type);
  #endif
            }
        
            // Delete response
            notecard.deleteResponse(rsp);
  #if DEBUG
          } else {
            notecard.logDebug("ERROR: Unrecognized Note format!\n");
  #endif
          }
  #if DEBUG
        } else {
          notecard.logDebug("ERROR: Unrecognized Note format!\n");
  #endif
        }
      }
      
      // Rearm ATTN Interrupt
      if (J *req = NoteNewRequest("card.attn")) {
        JAddStringToObject(req, "mode", "rearm,files");
        if (J *files = JAddArrayToObject(req, "files")) {
          JAddItemToArray(files, JCreateString("rob.qi"));
          if (!notecard.sendRequest(req)) {
#if DEBUG
            notecard.logDebug("ERROR: Failed to rearm ATTN interrupt!\n");
#endif
          }
        }
      }
    } else {
      // Await idle delay
      ::delay(IDLE_DELAY_MS);
      return;
    }
  }

  // Await modem
  if (modem_active) {
    //for(;modem_active;) {
      // check modem status
    //}
  }

  // Issue command
  if (rob.sendCommand(cmd)) {
#if DEBUG
    notecard.logDebug("ERROR: Failed to command R.O.B.!\n");
#endif
    return;
  }
  last_command_ms = millis();

  if (!soft_reset) {
    if (J *req = NoteNewRequest("note.get")) {
      JAddStringToObject(req, "file", "rob.qi");
      JAddBoolToObject(req, "delete", true);

      // Get Note
      if (!notecard.sendRequest(req)) {
#if DEBUG
        notecard.logDebug("ERROR: Failed to delete Note!\n");
#endif
      }
    }
  } else {
    soft_reset = false;
  }
}
