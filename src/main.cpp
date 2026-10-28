// Touch MIDI
// Five conductive-filament pads -> Bluetooth MIDI notes.
//
// How it works, in one paragraph:
//   Each pad is a capacitive touch sensor. The ESP32 measures a number for
//   each pad with touchRead(). On the original ESP32 that number gets SMALLER
//   when a finger is on the pad (your finger adds capacitance). At startup we
//   record each pad's "untouched" number (its baseline). After that, we look
//   at how far the reading has dropped below the baseline, as a percentage.
//   A big enough drop = a touch = MIDI note-on. Lifting your finger = note-off.

#include <Arduino.h>
#include <BLEMidi.h>

// ---------------------------------------------------------------------------
// Settings - change these to taste
// ---------------------------------------------------------------------------

const int NUM_PADS = 5;

// Which GPIO each pad is wired to (Button 1 ... Button 5).
const int PAD_PINS[NUM_PADS]  = {15, 4, 14, 12, 13};

// Which MIDI note each pad plays. 60 = middle C.
// C major pentatonic: C D E G A - any combination sounds good together.
const int PAD_NOTES[NUM_PADS] = {60, 62, 64, 67, 69};

// MIDI channel. The library counts from 0, so 0 here is "channel 1" in a DAW.
const int MIDI_CHANNEL = 0;

// Every note plays at this velocity (1-127).
const int NOTE_VELOCITY = 100;

// A pad counts as TOUCHED when its reading drops this many percent below
// its baseline...
const float TOUCH_ON_PERCENT  = 30.0;
// ...and counts as RELEASED only once it comes back above this.
// The gap between the two (hysteresis) stops a noisy reading near the
// threshold from rapidly flickering the note on and off.
const float TOUCH_OFF_PERCENT = 20.0;

// MIDI messages are sent one at a time, at least this many milliseconds
// apart. Sending two in the same instant can run Bluetooth out of buffer
// space, and the second message is lost - which leaves a note stuck on.
const unsigned long MIDI_SEND_GAP_MS = 5;

// Set to true to print every pad's drop % to the Serial Monitor.
// Handy for tuning the thresholds above, but turn it off when playing.
const bool DEBUG_PRINT = false;

// ---------------------------------------------------------------------------
// Pad state - one of these per pad
// ---------------------------------------------------------------------------

struct Pad {
  int pin;         // GPIO number
  int note;        // MIDI note number
  float baseline;  // touchRead() value with nothing touching
  bool playing;    // true once note-on has been sent, until release
};

Pad pads[NUM_PADS];

// ---------------------------------------------------------------------------
// Outgoing MIDI queue
// ---------------------------------------------------------------------------

// Instead of sending MIDI the moment a pad changes, we add the message to
// this queue. sendQueuedMidi() then sends them one at a time, in order.
struct MidiMessage {
  uint8_t status;   // 0x90 = note-on, 0x80 = note-off (plus the channel)
  uint8_t note;
  uint8_t velocity;
};

const int QUEUE_SIZE = 32;
MidiMessage midiQueue[QUEUE_SIZE];
int queueStart = 0;           // index of the oldest waiting message
int queueCount = 0;           // how many messages are waiting
unsigned long lastSendMs = 0; // when we last sent one

// The Bluetooth MIDI connection. We send through it directly, instead of
// BLEMidiServer.noteOn()/noteOff(), because the library hides whether a
// send actually worked.
NimBLECharacteristic *midiCharacteristic = nullptr;

// Adds a message to the back of the queue.
void queueMidi(uint8_t status, uint8_t note, uint8_t velocity) {
  if (!BLEMidiServer.isConnected()) return; // nobody listening
  if (queueCount == QUEUE_SIZE) {
    Serial.println("MIDI queue full - message dropped");
    return;
  }
  int end = (queueStart + queueCount) % QUEUE_SIZE;
  midiQueue[end] = {status, note, velocity};
  queueCount++;
}

// Tries to send one message. Returns false if Bluetooth couldn't take it
// right now (usually because its buffers are still full).
bool trySendMidi(const MidiMessage &msg) {
  // Every BLE MIDI packet starts with two bytes holding a millisecond timestamp.
  unsigned long t = millis();
  uint8_t packet[5] = {
    (uint8_t)(0x80 | ((t >> 7) & 0x3F)),
    (uint8_t)(0x80 | (t & 0x7F)),
    msg.status, msg.note, msg.velocity
  };
  return midiCharacteristic->notify(packet, sizeof(packet));
}

// Called every loop: sends the oldest waiting message, if it's time.
void sendQueuedMidi() {
  if (!BLEMidiServer.isConnected()) {
    queueCount = 0; // connection gone - throw away anything waiting
    return;
  }
  if (queueCount == 0) return;
  if (millis() - lastSendMs < MIDI_SEND_GAP_MS) return;

  // Bluetooth busy? Leave the message at the front and try again on the next
  // loop (~1 ms later). Nothing behind it gets sent first, so a note-off can
  // never overtake its note-on.
  if (!trySendMidi(midiQueue[queueStart])) return;

  queueStart = (queueStart + 1) % QUEUE_SIZE;
  queueCount--;
  lastSendMs = millis();
}

// ---------------------------------------------------------------------------
// Helper functions
// ---------------------------------------------------------------------------

// How far below its baseline this pad is right now, in percent.
//   0%  = untouched
//   50% = reading is half of the baseline (a solid touch)
float readDrop(const Pad &pad) {
  float raw = touchRead(pad.pin);
  return (pad.baseline - raw) / pad.baseline * 100.0;
}

// Averages a lot of readings with hands OFF to find each pad's resting value.
void calibrateBaselines() {
  Serial.println("Calibrating - keep hands off the pads...");
  delay(500); // give you a moment to move your hands away

  for (int i = 0; i < NUM_PADS; i++) {
    float sum = 0;
    for (int n = 0; n < 100; n++) {
      sum += touchRead(pads[i].pin);
      delay(2);
    }
    pads[i].baseline = sum / 100.0;
    Serial.printf("  Pad %d (GPIO %d): baseline %.1f\n",
                  i + 1, pads[i].pin, pads[i].baseline);
  }
  Serial.println("Ready!");
}

// ---------------------------------------------------------------------------
// setup() runs once at power-on
// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);

  // Copy the settings into each pad and start them all silent.
  for (int i = 0; i < NUM_PADS; i++) {
    pads[i].pin = PAD_PINS[i];
    pads[i].note = PAD_NOTES[i];
    pads[i].playing = false;
  }

  // Start advertising as a Bluetooth MIDI device. This name is what
  // shows up in Audio MIDI Setup on your Mac.
  BLEMidiServer.begin("Touch MIDI");

  // The library doesn't put the name in the Bluetooth advertisement, so the
  // Mac shows a string of random characters instead. The advertisement is
  // already full with the MIDI service ID, so we send the name in the
  // "scan response" - a second packet the Mac asks for while it's scanning.
  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  advertising->stop();
  advertising->enableScanResponse(true);
  advertising->setName("Touch MIDI");
  advertising->start();

  // Find the MIDI connection the library created, so we can send through it.
  midiCharacteristic = NimBLEDevice::getServer()
      ->getServiceByUUID(NimBLEUUID("03b80e5a-ede8-4b33-a751-6ce34ec4c700"))
      ->getCharacteristic(NimBLEUUID("7772e5db-3868-4112-a1a9-f2669d106bf3"));

  calibrateBaselines();
}

// ---------------------------------------------------------------------------
// loop() runs over and over, forever
// ---------------------------------------------------------------------------

void loop() {
  for (int i = 0; i < NUM_PADS; i++) {
    Pad &pad = pads[i];  // "&" means we edit the real pad, not a copy
    float drop = readDrop(pad);

    if (DEBUG_PRINT) Serial.printf("%5.1f  ", drop);

    // Finger just arrived? Start the note straight away.
    if (!pad.playing && drop > TOUCH_ON_PERCENT) {
      queueMidi(0x90 | MIDI_CHANNEL, pad.note, NOTE_VELOCITY);
      Serial.printf("Pad %d ON   note %d\n", i + 1, pad.note);
      pad.playing = true;
    }
    // Finger lifted? Stop the note.
    else if (pad.playing && drop < TOUCH_OFF_PERCENT) {
      queueMidi(0x80 | MIDI_CHANNEL, pad.note, 0);
      Serial.printf("Pad %d OFF  note %d\n", i + 1, pad.note);
      pad.playing = false;
    }
  }

  if (DEBUG_PRINT) Serial.println();

  sendQueuedMidi();

  delay(1); // a tiny pause keeps the loop steady (~1000 checks per second)
}
