#include <Arduino.h>
#include <SoftwareSerial.h>
#include <DFRobotDFPlayerMini.h>
#include <avr/sleep.h>
#include <avr/eeprom.h>

#define EEPROM_CFG 1
#define EEPROM_FOLDER 2
#define EEPROM_TRACK 4

#define BUTTON_TOLERANCE 25
#define LONG_KEY_PRESS_TIME_MS 2000L
#define VOLUME_CHECK_INTERVAL_MS 200L
#define PLAY_DELAY_MS 500L
#define FADE_OUT_MS 3L * 1000L * 60L
#define READ_RETRIES 3

#define PIN_KEY A3
#define PIN_VOLUME A2
#define PIN_VOLUME_INTERNAL A1

#define NO_FOLDERS 11

SoftwareSerial softSerial(0, 1); // RX, TX
DFRobotDFPlayerMini player;

enum {
  MODE_NORMAL, MODE_SET_TIMER
} mode = MODE_NORMAL;

boolean loopPlaylist = false;
boolean continuousPlayWithinPlaylist = false;
boolean restartLastTrackOnStart = false;

float volFade = 1.0;
int vol = -1;
int key = -1;
unsigned long keyPressTimeMs = 0L;
unsigned long volumeHandledLastMs = 0L;

unsigned long sleepAtMs = 0L;
unsigned long offAtMs = 0L;

unsigned long nowMs;

int16_t curFolder = -1;
int16_t curTrack = -1;
int16_t curTrackFileNumber = -1;

unsigned long startTrackAtMs = 0L;

int maxTracks[NO_FOLDERS];

void turnOff() {
  volFade = 0.0;
  player.volume(0);
  delay(50);
  player.stop();
  delay(50);
  player.outputDevice(DFPLAYER_DEVICE_SLEEP);
  delay(50);
  player.disableDAC();
  delay(50);
  player.sleep();
  delay(200);

  while (1) {
    set_sleep_mode(SLEEP_MODE_PWR_DOWN);
    sleep_mode();
    delay(5000);
  }
}

void initDFPlayer(boolean reset = false) {
  if (reset) {
    player.setTimeOut(6000);
    player.reset();
    player.waitAvailable();
  }
  player.setTimeOut(1000);

  delay(50);

  player.EQ(DFPLAYER_EQ_NORMAL);
  player.outputDevice(DFPLAYER_DEVICE_SD);
  player.enableDAC();
}

void readConfig() {
  uint8_t cfg = eeprom_read_byte((uint8_t*) EEPROM_CFG);

  loopPlaylist = cfg & 1;
  continuousPlayWithinPlaylist = cfg & 2;
  restartLastTrackOnStart = cfg & 4;
}

void writeConfig() {
  uint8_t cfg = eeprom_read_byte((uint8_t*) EEPROM_CFG);

  cfg = (cfg & (0xff ^ 1)) | (loopPlaylist ? 1 : 0);
  cfg = (cfg & (0xff ^ 2)) | (continuousPlayWithinPlaylist ? 2 : 0);
  cfg = (cfg & (0xff ^ 4)) | (restartLastTrackOnStart ? 4 : 0);

  eeprom_update_byte((uint8_t*) EEPROM_CFG, cfg);
}

void readTrackInfo() {
  curFolder = (int16_t) eeprom_read_word((uint16_t*) EEPROM_FOLDER);
  if (curFolder < 0 || curFolder > NO_FOLDERS) {
    curFolder = -1;
    curTrack = -1;

    return;
  }

  curTrack = (int16_t) eeprom_read_word((uint16_t*) EEPROM_TRACK);
  if (curTrack < 0 || curTrack > maxTracks[curFolder - 1]) {
    curTrack = -1;
    curFolder = -1;
  }
}

void writeTrackInfo(int16_t folder, int16_t track) {
  eeprom_update_word((uint16_t*) EEPROM_FOLDER, (uint16_t) folder);
  eeprom_update_word((uint16_t*) EEPROM_TRACK, (uint16_t) track);
}

int readPlayerState(int retries) {
  int ret = -1;
  while (--retries >= 0 && ret == -1) {
    ret = player.readState();
  }
  return ret;
}

int readPlayerFileCountsInFolder(int folderNumber, int retries) {
  int ret = -1;
  while (--retries >= 0 && ret == -1) {
    ret = player.readFileCountsInFolder(folderNumber);
  }
  return ret;
}

int readPlayerCurrentFileNumber(int retries) {
  int ret = -1;
  while (--retries >= 0 && ret == -1) {
    ret = player.readCurrentFileNumber(DFPLAYER_DEVICE_SD);
  }
  return ret;
}

void playOrAdvertise(int fileNo) {
  int state = readPlayerState(READ_RETRIES);
  if (state >= 0) {
    if ((state & 1) == 1) {
      player.advertise(fileNo);
    } else {
      player.playMp3Folder(fileNo);
    }
  }
}

void playFolderOrNextInFolder(int folder, boolean loop = true) {
  startTrackAtMs = 0;

  if (curFolder != folder) {
    curFolder = folder;
    curTrack = 1;
  } else {
    if (++curTrack > maxTracks[folder - 1]) {
      curTrack = 1;

      if (!loop)
      {
        curTrack = maxTracks[folder - 1];
        return;
      }
    }
  }

  startTrackAtMs = millis() + PLAY_DELAY_MS;
}

void setup() {
  pinMode(PIN_VOLUME, INPUT);
  pinMode(PIN_VOLUME_INTERNAL, INPUT);
  pinMode(PIN_KEY, INPUT_PULLUP);

  readConfig();

  delay(50);

  softSerial.begin(9600);

  player.begin(softSerial, false, false); // disable ACK to work with MH2024K-24SS chips

  initDFPlayer();

  for (int i = 0; i < NO_FOLDERS; ++i) {
    maxTracks[i] = readPlayerFileCountsInFolder(i + 1, READ_RETRIES);
  }

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
      player.volume(vol);
    }
  }
}

inline void handleKeyPress() {
  int keyCurrent = analogRead(PIN_KEY);

  if (keyCurrent > 1000 && key > 0) {
    switch (mode) {
      case MODE_NORMAL:
        if (nowMs - keyPressTimeMs >= LONG_KEY_PRESS_TIME_MS && key == 11) {
          mode = MODE_SET_TIMER;
          playOrAdvertise(100);
          delay(1000);
        } else if (nowMs - keyPressTimeMs >= LONG_KEY_PRESS_TIME_MS && key == 1) {
          continuousPlayWithinPlaylist = !continuousPlayWithinPlaylist;
          playOrAdvertise(continuousPlayWithinPlaylist ? 200 : 201);
          writeConfig();
          delay(1000);
        } else if (nowMs - keyPressTimeMs >= LONG_KEY_PRESS_TIME_MS && key == 2) {
          loopPlaylist = !loopPlaylist;
          playOrAdvertise(loopPlaylist ? 300 : 301);
          writeConfig();
          delay(1000);
        } else if (nowMs - keyPressTimeMs >= LONG_KEY_PRESS_TIME_MS && key == 3) {
          restartLastTrackOnStart = !restartLastTrackOnStart;
          playOrAdvertise(restartLastTrackOnStart ? 400 : 401);
          writeConfig();
          writeTrackInfo(curFolder, curTrack);
          delay(1000);
        } else {
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
  } else if (keyCurrent <= 1000) {
    int keyOld = key;

    if (keyCurrent > 949 - BUTTON_TOLERANCE) {
      key = 11;
    } else if (keyCurrent > 894 - BUTTON_TOLERANCE) {
      key = 9;
    } else if (keyCurrent > 847 - BUTTON_TOLERANCE) {
      key = 6;
    } else if (keyCurrent > 801 - BUTTON_TOLERANCE) {
      key = 3;
    } else if (keyCurrent > 754 - BUTTON_TOLERANCE) {
      key = 2;
    } else if (keyCurrent > 701 - BUTTON_TOLERANCE) {
      key = 5;
    } else if (keyCurrent > 635 - BUTTON_TOLERANCE) {
      key = 8;
    } else if (keyCurrent > 553 - BUTTON_TOLERANCE) {
      key = 10;
    } else if (keyCurrent > 444 - BUTTON_TOLERANCE) {
      key = 7;
    } else if (keyCurrent > 286 - BUTTON_TOLERANCE) {
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
  handleKeyPress();

  if (startTrackAtMs != 0 and nowMs >= startTrackAtMs) {
    startTrackAtMs = 0;
    player.playFolder(curFolder, curTrack);
    if (restartLastTrackOnStart) {
      writeTrackInfo(curFolder, curTrack);
    }

    // Don't reduce the following delay. Otherwise player might not have started playing
    // the requested track, returning the file number of the previous file, thus breaking
    // continuous play list playing which relies on correct curTrackFileNumber.
    delay(500);
    curTrackFileNumber = readPlayerCurrentFileNumber(READ_RETRIES);
  }

  if (player.available()) {
    uint8_t type = player.readType();
    int value = player.read();

    if (type == DFPlayerPlayFinished && value == curTrackFileNumber) {
      int16_t oldTrack = curTrack;

      if (startTrackAtMs == 0 /* no user request pending */ && continuousPlayWithinPlaylist) {
        playFolderOrNextInFolder(curFolder, loopPlaylist);

        // Don't reduce the following delay. Callbacks that a track has finished
        // might occur multiple times within 1 second and you don't want to move
        // on more than exactly one track.
        delay(1000);
      }

      if (oldTrack == curTrack) {
        writeTrackInfo(-1, -1);
      }
    }
  }

  delay(50);
}


