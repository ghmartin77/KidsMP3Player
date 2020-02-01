#include <Arduino.h>
#include <SoftwareSerial.h>
#include <avr/sleep.h>
#include <avr/eeprom.h>
// #define DEBUG_DFPLAYER_COMMUNICATION
#include "DFMiniMp3.h"

#define EEPROM_CFG 1
#define EEPROM_FOLDER 2
#define EEPROM_TRACK 4
#define EEPROM_INIT_MAGIC 8

#define MAGIC_MARKER 0xCAFE

#define BUTTON_TOLERANCE 25
#define LONG_KEY_PRESS_TIME_MS 2000L
#define VOLUME_CHECK_INTERVAL_MS 200L
#define PLAY_DELAY_MS 500L
#define FADE_OUT_MS 3L * 1000L * 60L

#define PIN_KEY A3
#define PIN_VOLUME A2
#define PIN_VOLUME_INTERNAL A1

// Uncomment to use button 11 as button "previous"
// #define USE_PREVIOUS_BUTTON

// Uncomment to make player pause playback on minimum volume
// #define PAUSE_ON_MIN_VOLUME

// Comment if compiler complains about too big binary
#define INIT_CONFIG_ON_FIRST_START

#define NO_FOLDERS 11
#ifdef USE_PREVIOUS_BUTTON
#define NO_FOLDERS 10
#endif

class Mp3Notify;

void initDFPlayer();
void playFolderOrNextInFolder(int folder, boolean loop);
void repeatCurrentOrPlayPreviousInCurFolder();
void writeTrackInfo(int16_t folder, int16_t track);
void readFolderTrackCounts();
boolean isPlaying();

SoftwareSerial softSerial(0, 1); // RX, TX

enum {
  MODE_NORMAL, MODE_SET_TIMER
} mode = MODE_NORMAL;

boolean continuousPlayWithinPlaylist = true;
boolean loopPlaylist = false;
boolean restartLastTrackOnStart = true;
boolean repeat1 = false;

boolean* config[] {&continuousPlayWithinPlaylist, &loopPlaylist, &restartLastTrackOnStart, &repeat1};

boolean paused = false;
boolean volatile error = false;
boolean sdAvailable = false;
boolean usbAvailable = false;

float volFade = 1.0;
int vol = -1;
int key = -1;
unsigned long keyPressTimeMs = 0L;
unsigned long volumeHandledLastMs = 0L;

unsigned long sleepAtMs = 0L;
unsigned long offAtMs = 0L;
unsigned long trackPlaybackStartedAtMs = 0L;

unsigned long nowMs;

int16_t curFolder = -1;
int16_t curTrack = -1;

int16_t expectedGlobalTrackToFinish = -1;

unsigned long startTrackAtMs = 0L;

int maxTracks[NO_FOLDERS];

DFMiniMp3<SoftwareSerial, Mp3Notify> player(softSerial);

class Mp3Notify
{
  public:
    static void OnError(uint16_t errorCode) {
      error = true;
    }

    static void OnPlayFinished(uint16_t globalTrack) {
      if (expectedGlobalTrackToFinish == globalTrack) {
        expectedGlobalTrackToFinish = -1;

        if (startTrackAtMs == 0 /* no user request pending */) {
          if (repeat1) {
            startTrackAtMs = millis() + PLAY_DELAY_MS;
          }
          else if (continuousPlayWithinPlaylist) {
            playFolderOrNextInFolder(curFolder, loopPlaylist);
          }
        }

        if (restartLastTrackOnStart && (curTrack == -1 || !continuousPlayWithinPlaylist)) {
          writeTrackInfo(-1, -1);
        }
      }
    }

    static void OnCardOnline(uint16_t code) {
      sdAvailable = true;
    }

    static void OnCardInserted(uint16_t code) {
      player.setPlaybackSource(DfMp3_PlaySource_Sd);
      readFolderTrackCounts();
    }

    static void OnCardRemoved(uint16_t code) {
      if (!isPlaying()) {
        player.setPlaybackSource(DfMp3_PlaySource_Usb);
        readFolderTrackCounts();
      }
    }

    static void OnUsbOnline(uint16_t code) {
      usbAvailable = true;
    }

    static void OnUsbInserted(uint16_t code) {
      player.setPlaybackSource(DfMp3_PlaySource_Usb);
      readFolderTrackCounts();
    }

    static void OnUsbRemoved(uint16_t code) {
      if (!isPlaying()) {
        player.setPlaybackSource(DfMp3_PlaySource_Sd);
        readFolderTrackCounts();
      }
    }
};

void turnOff() {
  volFade = 0.0;
  player.setVolume(0);
  delay(50);
  player.stop();
  delay(50);
  player.setPlaybackSource(DfMp3_PlaySource_Sleep);
  delay(50);
  player.disableDac();
  delay(50);
  player.sleep();
  delay(200);

  while (1) {
    set_sleep_mode(SLEEP_MODE_PWR_DOWN);
    sleep_mode();
    delay(5000);
  }
}

void initDFPlayer() {
  delay(100);
  player.setEq(DfMp3_Eq_Normal);
  delay(100);

  if (usbAvailable) {
    player.setPlaybackSource(DfMp3_PlaySource_Usb);
  } else {
    player.setPlaybackSource(DfMp3_PlaySource_Sd);
  }
  delay(250);

  player.enableDac();
  delay(250);
}

void readConfig() {
  uint8_t cfg = eeprom_read_byte((uint8_t*) EEPROM_CFG);

  loopPlaylist = cfg & 1;
  continuousPlayWithinPlaylist = cfg & 2;
  restartLastTrackOnStart = cfg & 4;
  repeat1 = cfg & 8;
}

void writeConfig() {
  uint8_t cfg = eeprom_read_byte((uint8_t*) EEPROM_CFG);

  cfg = (cfg & (0xff ^ 1)) | (loopPlaylist ? 1 : 0);
  cfg = (cfg & (0xff ^ 2)) | (continuousPlayWithinPlaylist ? 2 : 0);
  cfg = (cfg & (0xff ^ 4)) | (restartLastTrackOnStart ? 4 : 0);
  cfg = (cfg & (0xff ^ 8)) | (repeat1 ? 8 : 0);

  eeprom_update_byte((uint8_t*) EEPROM_CFG, cfg);
}

void readTrackInfo() {
  curFolder = (int16_t) eeprom_read_word((uint16_t*) EEPROM_FOLDER);
  if (curFolder < 1 || curFolder > NO_FOLDERS) {
    curFolder = -1;
    curTrack = -1;

    return;
  }

  curTrack = (int16_t) eeprom_read_word((uint16_t*) EEPROM_TRACK);
  if (curTrack < 1 || curTrack > maxTracks[curFolder - 1]) {
    curTrack = -1;
    curFolder = -1;
  }
}

void writeTrackInfo(int16_t folder, int16_t track) {
  eeprom_update_word((uint16_t*) EEPROM_FOLDER, (uint16_t) folder);
  eeprom_update_word((uint16_t*) EEPROM_TRACK, (uint16_t) track);
}

boolean isPlaying() {
  uint16_t state = 0;
  int retries = 3;

  error = true;
  while (retries > 0 && error) {
    --retries;
    error = false;
    state = player.getStatus();
    if (error) {
      delay(100);
    }
  }
  error = false;

  return (state & 1) == 1;
}

void playOrAdvertise(int fileNo) {
  if (isPlaying()) {
    player.playAdvertisement(fileNo);
  } else {
    player.playMp3FolderTrack(fileNo);
  }
}

void playFolderOrNextInFolder(int folder, boolean loop = true) {
  startTrackAtMs = 0;

  if (curFolder != folder) {
    curFolder = folder;
    curTrack = 1;
  } else {
    if (curTrack == -1) {
      curTrack = 1;
    }
    else if (++curTrack > maxTracks[folder - 1]) {
      if (!loop)
      {
        curTrack = -1;
        return;
      }

      curTrack = 1;
    }
  }

  startTrackAtMs = millis() + PLAY_DELAY_MS;
}

void repeatCurrentOrPlayPreviousInCurFolder() {
  if (curFolder == -1 || curTrack == -1) {
    return;
  }

  startTrackAtMs = 0;

  if (trackPlaybackStartedAtMs == 0 || millis() < trackPlaybackStartedAtMs + 3000) {
    if (--curTrack <= 0) {
      curTrack = maxTracks[curFolder - 1];
    }
  }

  trackPlaybackStartedAtMs = 0;
  startTrackAtMs = millis() + PLAY_DELAY_MS;
}

void readFolderTrackCounts() {
  delay(2000);

  for (int i = 0; i < NO_FOLDERS; ++i) {
    maxTracks[i] = player.getFolderTrackCount(i + 1);
    if (maxTracks[i] == -1) i--;
  }

  curFolder = -1;
  curTrack = -1;
}

#ifdef INIT_CONFIG_ON_FIRST_START
void initConfigOnFirstStart() {
  uint16_t magic = eeprom_read_word((uint16_t*) EEPROM_INIT_MAGIC);

  if (magic != MAGIC_MARKER) {
    eeprom_update_word((uint16_t*) EEPROM_INIT_MAGIC, MAGIC_MARKER);
    writeConfig();
    writeTrackInfo(-1, -1);
  }
}
#endif

void setup() {
  player.begin();

  pinMode(PIN_VOLUME, INPUT);
  pinMode(PIN_VOLUME_INTERNAL, INPUT);
  pinMode(PIN_KEY, INPUT_PULLUP);

#ifdef INIT_CONFIG_ON_FIRST_START
  initConfigOnFirstStart();
#endif
  readConfig();

  delay(1500);

  error = false;
  uint8_t storageDevices = player.queryStorageDevices();
  if (error) {
    error = false;
    if (!sdAvailable && !usbAvailable) {
      player.reset();
      for (int i = 0; i < 30; ++i) {
        player.loop();
        delay(50);
      }
    }
  }
  else {
    usbAvailable = storageDevices & 0x01;
    sdAvailable = storageDevices & 0x02;
  }

  initDFPlayer();

  readFolderTrackCounts();

  if (restartLastTrackOnStart) {
    readTrackInfo();
    if (curFolder != -1 && curTrack != -1) {
      startTrackAtMs = millis() + PLAY_DELAY_MS;
    }
  }
}

inline void handleSleepTimer() {
  if (sleepAtMs != 0 && nowMs >= sleepAtMs) {
    volFade = 1.0 - (nowMs - sleepAtMs) / (float) (offAtMs - sleepAtMs);
    if (volFade <= 0.0) {
      turnOff();
    }
  }
}

inline void handleVolume() {
  if (nowMs > volumeHandledLastMs + VOLUME_CHECK_INTERVAL_MS) {
    volumeHandledLastMs = nowMs;

    int volCurrent = analogRead(PIN_VOLUME);
    int volInternal = analogRead(PIN_VOLUME_INTERNAL);
    int volNew = (map(volCurrent, 0, 1023, 1,
                      31 - map(volInternal, 1023, 0, 1, 30))) * volFade;
    if (volNew != vol) {
      vol = volNew;
      player.setVolume(vol);

#ifdef PAUSE_ON_MIN_VOLUME
      if (vol == 1 && !paused) {
        paused = true;
        player.pause();
      }
      if (vol > 1 && paused) {
        paused = false;
        player.start();
      }
#endif
    }
  }
}

inline void handleKeyPress() {
  int keyCurrent = analogRead(PIN_KEY);

  if (keyCurrent > 958 && key > 0) {
    switch (mode) {
      case MODE_NORMAL:
        if (nowMs - keyPressTimeMs >= LONG_KEY_PRESS_TIME_MS && key == 11) {
          mode = MODE_SET_TIMER;
          playOrAdvertise(100);
          delay(1000);
        } else if (nowMs - keyPressTimeMs >= LONG_KEY_PRESS_TIME_MS && key >= 1 && key <= 4) {
          *config[key-1] = !*config[key-1];
          playOrAdvertise(*config[key-1] ? (key + 1) * 100 : (key + 1) * 100 + 1);
          writeConfig();
          if (key == 3) {
            writeTrackInfo(restartLastTrackOnStart ? curFolder : -1, restartLastTrackOnStart ? curTrack : -1);
          }
          delay(1000);
        } else {
#ifdef USE_PREVIOUS_BUTTON
          if (key == 11) {
            repeatCurrentOrPlayPreviousInCurFolder();
          }
          else
#endif
            playFolderOrNextInFolder(key);
        }
        break;

      case MODE_SET_TIMER:
        playOrAdvertise(key);

        if (key == 1) {
          sleepAtMs = 0;
          offAtMs = 0;
          volFade = 1.0;
        } else {
          sleepAtMs = nowMs + (key - 1) * 1000L * 60L * 5L;
          offAtMs = sleepAtMs + FADE_OUT_MS;
        }

        delay(1000);
        mode = MODE_NORMAL;
        break;

    }

    key = -1;
  } else if (keyCurrent <= 958) {
    int keyOld = key;

    if (keyCurrent > 933 - BUTTON_TOLERANCE) {
      key = 11;
    } else if (keyCurrent > 846 - BUTTON_TOLERANCE) {
      key = 9;
    } else if (keyCurrent > 760 - BUTTON_TOLERANCE) {
      key = 6;
    } else if (keyCurrent > 676 - BUTTON_TOLERANCE) {
      key = 3;
    } else if (keyCurrent > 590 - BUTTON_TOLERANCE) {
      key = 2;
    } else if (keyCurrent > 504 - BUTTON_TOLERANCE) {
      key = 5;
    } else if (keyCurrent > 414 - BUTTON_TOLERANCE) {
      key = 8;
    } else if (keyCurrent > 321 - BUTTON_TOLERANCE) {
      key = 10;
    } else if (keyCurrent > 222 - BUTTON_TOLERANCE) {
      key = 7;
    } else if (keyCurrent > 115 - BUTTON_TOLERANCE) {
      key = 4;
    } else if (keyCurrent > 0) {
      key = 1;
    }

    if (keyOld != key) {
      keyPressTimeMs = nowMs;
    }
  }
}

void loop() {
  nowMs = millis();

  handleSleepTimer();
  handleVolume();
  if (!paused) {
    handleKeyPress();
  }

  if (startTrackAtMs != 0 and nowMs >= startTrackAtMs) {
    startTrackAtMs = 0;
    trackPlaybackStartedAtMs = nowMs;
    player.playFolderTrack(curFolder, curTrack);
    if (restartLastTrackOnStart) {
      writeTrackInfo(curFolder, curTrack);
    }

    // Don't reduce the following delay. Otherwise player might not have started playing
    // the requested track, returning the file number of the previous file, thus breaking
    // continuous play list playing which relies on correct curTrackFileNumber.
    delay(500);
    expectedGlobalTrackToFinish = player.getCurrentTrack();
  }

  player.loop();

  if (softSerial.overflow()) {
    softSerial.flush();
  }

  delay(50);
}



