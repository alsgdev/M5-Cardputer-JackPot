/*
  Jackpot de Bolso - caca-niquel para M5Stack Cardputer (v3)

  Placa: M5Cardputer (pacote de placas M5Stack)
  Bibliotecas: M5Cardputer, M5Unified, M5GFX (Preferences vem no core ESP32)

  Telas:cccccbnfnkehdigrlg
    MENU            JOGAR | CONFIGURACOES
    CONFIGURACOES   VOLUME | SOM | BRILHO | IDIOMA | ZERAR PLACAR | VOLTAR
    JOGO            caca-niquel com 3 rolos de frutas
    GAME OVER       aparece por 4 segundos e recarrega 100 creditos

  Teclas de navegacao (setas impressas no teclado do Cardputer):
    ;  cima     .  baixo     ,  esquerda     /  direita
    ENTER confirma           `  (ESC) volta

  No menu:     ; .  escolhem    ENTER abre    1 = jogar    2 = configuracoes
  Nas config.: ; .  escolhem    , /  ajustam  ENTER alterna/confirma  ` volta
  No jogo:
    [  ou  ,    diminui o volume       ]  ou  /    aumenta o volume
    -  ou  _    diminui a aposta       =  ou  +    aumenta a aposta
    m           liga/desliga o som     `           volta ao menu
    qualquer outra tecla gira (durante a comemoracao, encerra a animacao)

  Tudo e salvo na flash (NVS) e sobrevive ao desligamento:
    creditos, recorde, giros, vitorias, jackpots, aposta,
    volume, som ligado/desligado, brilho e idioma (PT-BR / EN).

  Regras (multiplicadores da aposta, ja incluindo a aposta devolvida):
    par = 1x | trinca = 10x | 7-7-7 = 50x
  Inicio com 100 creditos. Ao chegar a zero, a tela GAME OVER aparece
  por 4 segundos e o jogo recarrega 100 creditos automaticamente.
  Creditos ficticios, sem valor real.

  Referencias de API:
  https://docs.m5stack.com/en/arduino/m5cardputer/program
  https://docs.m5stack.com/en/arduino/m5cardputer/keyboard
  https://docs.m5stack.com/en/arduino/m5cardputer/display
  https://raw.githubusercontent.com/m5stack/M5Unified/master/src/utility/Speaker_Class.hpp
*/
#include <Arduino.h>
#include <M5Cardputer.h>
#include <M5GFX.h>
#include <Preferences.h>
#include <esp_system.h>
#include <math.h>

// ---------------------------------------------------------------- Config
constexpr bool ENABLE_FLASH = true;
constexpr int VOLUME_STEPS = 10;           // niveis 0..10
constexpr int DEFAULT_VOLUME = 4;
constexpr int BRIGHT_MIN = 1;              // niveis 1..10 (0 apagaria a tela)
constexpr int BRIGHT_STEPS = 10;
constexpr int DEFAULT_BRIGHT = 7;
constexpr int32_t START_CREDITS = 100;
constexpr int32_t MAX_CREDITS = 999999;
constexpr int32_t BET_OPTIONS[] = {1, 2, 5, 10, 25};
constexpr int BET_OPTION_COUNT = sizeof(BET_OPTIONS) / sizeof(BET_OPTIONS[0]);
constexpr int DEFAULT_BET_INDEX = 2;       // 5 creditos
constexpr int32_t PAY_PAIR = 1;
constexpr int32_t PAY_TRIPLE = 10;
constexpr int32_t PAY_JACKPOT = 50;
constexpr uint32_t FRAME_MS = 20;          // ~50 quadros/s
constexpr uint32_t CELEBRATION_MS = 2300;
constexpr uint32_t GAME_OVER_MS = 4000;
constexpr uint32_t FLASH_PHASE_MS = 180;
constexpr uint32_t RESET_CONFIRM_MS = 3000;
constexpr uint32_t INFO_ROTATE_MS = 3000;
constexpr uint32_t TOAST_MS = 1500;

// ---------------------------------------------------------------- Layout
constexpr int REEL_COUNT = 3;
constexpr int SEQUENCE_LENGTH = 64;
constexpr int REEL_W = 66;
constexpr int REEL_H = 54;
constexpr int REEL_Y = 43;
constexpr int MENU_REEL_Y = 25;
constexpr int REEL_X[REEL_COUNT] = {13, 87, 161};
constexpr int CELL_H = 48;

enum Symbol : uint8_t { SYM_CHERRY, SYM_LEMON, SYM_ORANGE, SYM_GRAPE, SYM_BELL, SYM_SEVEN, SYMBOL_COUNT };

// ---------------------------------------------------------------- Cores RGB565
const uint16_t COLORS[] = {0xF81F, 0x07FF, 0xFFE0, 0x07E0, 0xFD20, 0xF800};
constexpr int COLOR_COUNT = sizeof(COLORS) / sizeof(COLORS[0]);
constexpr uint16_t BG = 0x0843;
constexpr uint16_t REEL_BG = 0x1082;
constexpr uint16_t C_GOLD = 0xFEA0;
constexpr uint16_t C_WHITE = 0xFFFF;
constexpr uint16_t C_GRAY = 0x8410;
constexpr uint16_t C_CYAN = 0x07FF;
constexpr uint16_t C_BLACK = 0x0000;
constexpr uint16_t C_GREEN = 0x07E0;
constexpr uint16_t C_DKGREEN = 0x0320;
constexpr uint16_t C_RED = 0xF800;
constexpr uint16_t C_DKRED = 0xA000;
constexpr uint16_t C_PANEL = 0x18E3;

M5Canvas screen(&M5Cardputer.Display);
M5Canvas reelCanvas(&M5Cardputer.Display);
Preferences prefs;

enum Screen { SCREEN_MENU, SCREEN_SETTINGS, SCREEN_GAME };
Screen currentScreen = SCREEN_MENU;
enum GameState { READY, SPINNING, CELEBRATING, GAME_OVER };
GameState state = READY;

enum MenuItem { MENU_PLAY, MENU_SETTINGS, MENU_COUNT };
enum SettingItem { SET_VOLUME, SET_SOUND, SET_BRIGHT, SET_LANG, SET_RESET, SET_BACK, SET_COUNT };
int menuIndex = 0;
int settingIndex = 0;

struct Reel {
  uint8_t sequence[SEQUENCE_LENGTH];
  uint8_t finalSymbol = 0;
  uint8_t colorOffset = 0;
  int steps = 0;
  uint32_t duration = 0;
  float position = 0;
  bool stopped = true;
};
Reel reels[REEL_COUNT];
Reel demoReels[REEL_COUNT];                // rolos decorativos do menu

// Estado persistente: placar
int32_t credits = START_CREDITS;
int32_t best = START_CREDITS;
uint32_t spins = 0, wins = 0, jackpots = 0;
int betIndex = DEFAULT_BET_INDEX;
// Estado persistente: configuracoes
int volumeLevel = DEFAULT_VOLUME;
int brightLevel = DEFAULT_BRIGHT;
bool muted = false;

// ---------------------------------------------------------------- Idioma
enum Language : uint8_t { LANG_PT, LANG_EN, LANG_COUNT };
Language language = LANG_PT;
const char* const LANGUAGE_NAMES[LANG_COUNT] = {"PT-BR", "ENGLISH"};

enum TextId : uint8_t {
  TXT_TITLE, TXT_PLAY, TXT_SETTINGS, TXT_MENU_HINT, TXT_MENU_STATS,
  TXT_VOLUME, TXT_SOUND, TXT_BRIGHTNESS, TXT_LANGUAGE, TXT_RESET_SCORE, TXT_BACK,
  TXT_ON, TXT_OFF, TXT_ENTER, TXT_ENTER_AGAIN, TXT_SETTINGS_HINT,
  TXT_GAME_HEADER, TXT_MUTE, TXT_PAY_TABLE, TXT_CONTROLS, TXT_STATS,
  TXT_READY, TXT_GOOD_LUCK, TXT_JACKPOT, TXT_TRIPLE, TXT_PAIR, TXT_NO_WIN,
  TXT_NO_CREDITS, TXT_BET_TOAST, TXT_VOLUME_TOAST, TXT_SOUND_OFF, TXT_SOUND_ON,
  TXT_BRIGHT_TOAST, TXT_SCORE_RESET, TXT_GAME_OVER, TXT_REFILLING, TXT_REFILL_INFO,
  TXT_LANGUAGE_TOAST, TXT_SETUP_ERROR, TXT_COUNT
};

const char* const TEXT[LANG_COUNT][TXT_COUNT] = {
  { // PT-BR
    "JACKPOT DE BOLSO", "JOGAR", "CONFIGURACOES",
    "; . ESCOLHE   ENTER CONFIRMA",
    "REC:%ld  CR:%ld  JACKPOTS:%lu",
    "VOLUME", "SOM", "BRILHO", "IDIOMA", "ZERAR PLACAR", "VOLTAR",
    "LIGADO", "DESLIGADO", "ENTER", "ENTER DE NOVO",
    ";. ITEM  ,/ AJUSTA  ` VOLTA",
    "CR:%ld REC:%ld APOSTA:%ld", "MUDO",
    "PAR:%ldx  TRINCA:%ldx  777:%ldx",
    "-= APOSTA [] VOL M:SOM `:MENU",
    "GIROS:%lu VIT:%lu JACKPOTS:%lu",
    "QUALQUER TECLA GIRA", "BOA SORTE!", "JACKPOT!!", "TRINCA!", "PAR!",
    "NADA... TENTE DE NOVO", "SEM CREDITOS",
    "APOSTA: %ld", "VOLUME %d/%d", "SOM DESLIGADO", "SOM LIGADO",
    "BRILHO %d/%d", "PLACAR ZERADO", "GAME OVER",
    "RECARREGANDO EM %d...", "NOVA RECARGA: %ld CREDITOS",
    "IDIOMA: %s", "Falha ao alocar sprites.\nReinicie o aparelho."
  },
  { // English
    "POCKET JACKPOT", "PLAY", "SETTINGS",
    "; . SELECT  ENTER CONFIRM",
    "BEST:%ld  CR:%ld  JACKPOTS:%lu",
    "VOLUME", "SOUND", "BRIGHTNESS", "LANGUAGE", "RESET SCORE", "BACK",
    "ON", "OFF", "ENTER", "ENTER AGAIN",
    ";. ITEM  ,/ ADJUST  ` BACK",
    "CR:%ld BEST:%ld BET:%ld", "MUTE",
    "PAIR:%ldx  TRIPLE:%ldx  777:%ldx",
    "-= BET [] VOL M:SND `:MENU",
    "SPINS:%lu WINS:%lu JACKPOTS:%lu",
    "PRESS ANY KEY TO SPIN", "GOOD LUCK!", "JACKPOT!!", "TRIPLE!", "PAIR!",
    "NO WIN... TRY AGAIN", "NO CREDITS",
    "BET: %ld", "VOLUME %d/%d", "SOUND OFF", "SOUND ON",
    "BRIGHTNESS %d/%d", "SCORE RESET", "GAME OVER",
    "REFILLING IN %d...", "REFILL: %ld CREDITS",
    "LANGUAGE: %s", "Sprite allocation failed.\nPlease restart the device."
  }
};

inline const char* tr(TextId id) { return TEXT[language][id]; }

// Estado de execucao
int32_t lastPayout = 0;
uint8_t winMask = 0;                       // bits dos rolos vencedores
uint32_t spinStarted = 0, celebrationStarted = 0, gameOverStarted = 0;
uint32_t lastFrame = 0, lastTick = 0;
uint32_t resetArmedAt = 0;
bool resetArmed = false;
bool keyboardArmed = false;
char message[40] = "QUALQUER TECLA GIRA";
char toast[32] = "";                       // aviso rapido (volume, brilho...)
uint32_t toastAt = 0;

int32_t bet() { return BET_OPTIONS[betIndex]; }

void showToast(const char* text, uint32_t now) {
  snprintf(toast, sizeof(toast), "%s", text);
  toastAt = now;
}

// ---------------------------------------------------------------- Hardware
void applyVolume() {
  // 10 niveis -> 0..250 no volume mestre do alto-falante.
  M5Cardputer.Speaker.setVolume((uint8_t)(volumeLevel * 25));
}

void applyBrightness() {
  M5Cardputer.Display.setBrightness((uint8_t)(brightLevel * 25));
}

// ---------------------------------------------------------------- Persistencia
void loadAll() {
  prefs.begin("jackpot", false);
  credits = constrain(prefs.getInt("credits", START_CREDITS), 0, MAX_CREDITS);
  best = constrain(prefs.getInt("best", START_CREDITS), 0, MAX_CREDITS);
  spins = prefs.getUInt("spins", 0);
  wins = prefs.getUInt("wins", 0);
  jackpots = prefs.getUInt("jackpots", 0);
  betIndex = constrain((int)prefs.getUChar("bet", DEFAULT_BET_INDEX), 0, BET_OPTION_COUNT - 1);
  muted = prefs.getBool("muted", false);
  volumeLevel = constrain((int)prefs.getUChar("volume", DEFAULT_VOLUME), 0, VOLUME_STEPS);
  brightLevel = constrain((int)prefs.getUChar("bright", DEFAULT_BRIGHT), BRIGHT_MIN, BRIGHT_STEPS);
  language = (Language)constrain((int)prefs.getUChar("lang", LANG_PT), 0, LANG_COUNT - 1);
  if (best < credits) best = credits;
}

void saveStats() {
  // A NVS so grava quando o valor muda, reduzindo desgaste da flash.
  prefs.putInt("credits", credits);
  prefs.putInt("best", best);
  prefs.putUInt("spins", spins);
  prefs.putUInt("wins", wins);
  prefs.putUInt("jackpots", jackpots);
  prefs.putUChar("bet", (uint8_t)betIndex);
}

void saveSettings() {
  prefs.putUChar("volume", (uint8_t)volumeLevel);
  prefs.putUChar("bright", (uint8_t)brightLevel);
  prefs.putBool("muted", muted);
  prefs.putUChar("lang", (uint8_t)language);
}

void resetStats() {
  credits = START_CREDITS;
  best = START_CREDITS;
  spins = wins = jackpots = 0;
  betIndex = DEFAULT_BET_INDEX;
  saveStats();
}

// ---------------------------------------------------------------- Som
const uint16_t WIN_NOTES[] = {523, 659, 784, 1047, 784, 1047};
const uint16_t JACKPOT_NOTES[] = {523, 659, 784, 1047, 1319, 1568, 2093, 1568};
uint8_t melodyIndex = 0, melodyLength = 0;
uint32_t lastNote = 0;
bool jackpotMelody = false;

void playTone(uint16_t frequency, uint32_t duration, int channel = 0) {
  if (!muted && volumeLevel > 0) M5Cardputer.Speaker.tone(frequency, duration, channel, true);
}

void startMelody(bool jackpot, uint32_t now) {
  jackpotMelody = jackpot;
  melodyIndex = 0;
  melodyLength = jackpot ? 8 : 6;
  lastNote = now - 150;
}

void updateAudio(uint32_t now) {
  if (!melodyLength || uint32_t(now - lastNote) < 150) return;
  lastNote = now;
  playTone(jackpotMelody ? JACKPOT_NOTES[melodyIndex] : WIN_NOTES[melodyIndex], 115);
  if (++melodyIndex >= melodyLength) melodyLength = 0;
}

// Ajustes compartilhados entre o jogo e a tela de configuracoes.
void changeVolume(int delta, uint32_t now) {
  volumeLevel = constrain(volumeLevel + delta, 0, VOLUME_STEPS);
  if (delta > 0 && muted) muted = false;   // aumentar o volume tira o mudo
  applyVolume();
  saveSettings();
  char t[24];
  snprintf(t, sizeof(t), tr(TXT_VOLUME_TOAST), volumeLevel, VOLUME_STEPS);
  showToast(t, now);
  playTone(880, 50);                       // amostra do novo volume
}

void toggleMute(uint32_t now) {
  muted = !muted;
  if (muted) { M5Cardputer.Speaker.stop(); melodyLength = 0; }
  saveSettings();
  showToast(tr(muted ? TXT_SOUND_OFF : TXT_SOUND_ON), now);
  playTone(880, 60);
}

void changeBrightness(int delta, uint32_t now) {
  brightLevel = constrain(brightLevel + delta, BRIGHT_MIN, BRIGHT_STEPS);
  applyBrightness();
  saveSettings();
  char t[24];
  snprintf(t, sizeof(t), tr(TXT_BRIGHT_TOAST), brightLevel, BRIGHT_STEPS);
  showToast(t, now);
  playTone(delta > 0 ? 990 : 660, 30);
}

void changeLanguage(int delta, uint32_t now) {
  language = (Language)constrain((int)language + delta, 0, LANG_COUNT - 1);
  saveSettings();
  char t[24];
  snprintf(t, sizeof(t), tr(TXT_LANGUAGE_TOAST), LANGUAGE_NAMES[language]);
  showToast(t, now);
  playTone(880, 40);
}

// ---------------------------------------------------------------- Jogo
void enterGame(uint32_t now) {
  currentScreen = SCREEN_GAME;
  resetArmed = false;
  if (credits <= 0) {
    state = GAME_OVER;
    gameOverStarted = now;
    melodyLength = 0;
    snprintf(message, sizeof(message), "%s", tr(TXT_NO_CREDITS));
  } else {
    state = READY;
    snprintf(message, sizeof(message), "%s", tr(TXT_READY));
  }
}

void enterMenu() {
  currentScreen = SCREEN_MENU;
  melodyLength = 0;
  resetArmed = false;
}

void startGameOver(uint32_t now) {
  state = GAME_OVER;
  gameOverStarted = now;
  melodyLength = 0;
  snprintf(message, sizeof(message), "%s", tr(TXT_NO_CREDITS));
  playTone(180, 300);
}

void startSpin(uint32_t now) {
  if (credits < bet()) {
    // Reduz a aposta para o maior valor possivel; sem saldo, game over.
    while (betIndex > 0 && BET_OPTIONS[betIndex] > credits) --betIndex;
    if (credits < bet()) { startGameOver(now); return; }
  }
  credits -= bet();
  ++spins;
  lastPayout = 0;
  winMask = 0;
  melodyLength = 0;
  snprintf(message, sizeof(message), "%s", tr(TXT_GOOD_LUCK));
  spinStarted = now;
  lastTick = now;
  state = SPINNING;

  for (int i = 0; i < REEL_COUNT; ++i) {
    Reel& r = reels[i];
    r.steps = 22 + i * 9 + random(0, 6);   // maximo 45, cabe no vetor
    r.duration = 1700 + i * 650 + random(0, 200);
    r.position = 0;
    r.stopped = false;
    r.colorOffset = random(0, COLOR_COUNT);
    r.sequence[0] = r.finalSymbol;         // comeca do simbolo atual
    for (int n = 1; n < SEQUENCE_LENGTH; ++n) r.sequence[n] = random(0, SYMBOL_COUNT);
    r.finalSymbol = random(0, SYMBOL_COUNT);  // resultado independente da animacao
    r.sequence[r.steps] = r.finalSymbol;
  }
  saveStats();                             // aposta debitada fica registrada
  playTone(440, 65);
}

void evaluateSpin(uint32_t now) {
  uint8_t a = reels[0].finalSymbol, b = reels[1].finalSymbol, c = reels[2].finalSymbol;
  bool triple = (a == b && b == c);
  bool jackpot = triple && a == SYM_SEVEN;

  int32_t mult = 0;
  if (jackpot) { mult = PAY_JACKPOT; winMask = 0b111; }
  else if (triple) { mult = PAY_TRIPLE; winMask = 0b111; }
  else if (a == b) { mult = PAY_PAIR; winMask = 0b011; }
  else if (b == c) { mult = PAY_PAIR; winMask = 0b110; }
  else if (a == c) { mult = PAY_PAIR; winMask = 0b101; }

  lastPayout = mult * bet();
  credits = min(credits + lastPayout, MAX_CREDITS);
  if (credits > best) best = credits;

  if (lastPayout > 0) {
    ++wins;
    if (jackpot) ++jackpots;
    snprintf(message, sizeof(message), "%s  +%ld",
             jackpot ? tr(TXT_JACKPOT) : triple ? tr(TXT_TRIPLE) : tr(TXT_PAIR),
             (long)lastPayout);
    state = CELEBRATING;
    celebrationStarted = now;
    startMelody(jackpot, now);
  } else {
    snprintf(message, sizeof(message), "%s", tr(TXT_NO_WIN));
    state = READY;
    playTone(180, 180);
  }
  saveStats();
  if (credits <= 0) startGameOver(now);
}

void updateGame(uint32_t now) {
  if (resetArmed && uint32_t(now - resetArmedAt) >= RESET_CONFIRM_MS) resetArmed = false;
  if (currentScreen != SCREEN_GAME) return;

  if (state == GAME_OVER) {
    if (uint32_t(now - gameOverStarted) >= GAME_OVER_MS) {
      credits = START_CREDITS;
      state = READY;
      snprintf(message, sizeof(message), "%s", tr(TXT_READY));
      saveStats();
      playTone(880, 80);
    }
    return;
  }

  if (state == CELEBRATING) {
    if (uint32_t(now - celebrationStarted) >= CELEBRATION_MS) {
      state = READY;
      snprintf(message, sizeof(message), "%s", tr(TXT_READY));
    }
    return;
  }
  if (state != SPINNING) return;

  uint32_t elapsed = now - spinStarted;
  bool allStopped = true;
  for (int i = 0; i < REEL_COUNT; ++i) {
    Reel& r = reels[i];
    if (r.stopped) continue;
    if (elapsed >= r.duration) {
      r.position = float(r.steps);
      r.stopped = true;
      playTone(650 + i * 220, 70);
    } else {
      float remaining = 1.0f - float(elapsed) / float(r.duration);
      r.position = float(r.steps) * (1.0f - remaining * remaining * remaining);  // ease-out cubico
      allStopped = false;
    }
  }
  if (allStopped) {
    evaluateSpin(now);
  } else if (uint32_t(now - lastTick) >= 105) {
    lastTick = now;
    playTone(220 + random(0, 160), 12, 1);  // canal 1 preserva os tons de parada
  }
}

// ---------------------------------------------------------------- Teclado
// Setas impressas no teclado do Cardputer.
inline bool isUp(char c)    { return c == ';'; }
inline bool isDown(char c)  { return c == '.'; }
inline bool isLeft(char c)  { return c == ','; }
inline bool isRight(char c) { return c == '/'; }
inline bool isEsc(char c)   { return c == '`' || c == '~'; }

void handleMenuKey(char ch, const Keyboard_Class::KeysState& ks, uint32_t now) {
  if (isUp(ch) || isLeft(ch))   { menuIndex = (menuIndex + MENU_COUNT - 1) % MENU_COUNT; playTone(700, 25); return; }
  if (isDown(ch) || isRight(ch)) { menuIndex = (menuIndex + 1) % MENU_COUNT; playTone(700, 25); return; }
  if (ch == '1') menuIndex = MENU_PLAY;
  else if (ch == '2') menuIndex = MENU_SETTINGS;
  else if (!ks.enter && ch != ' ') return;

  playTone(1047, 60);
  if (menuIndex == MENU_PLAY) enterGame(now);
  else { currentScreen = SCREEN_SETTINGS; settingIndex = 0; resetArmed = false; }
}

void handleSettingsKey(char ch, const Keyboard_Class::KeysState& ks, uint32_t now) {
  if (isEsc(ch) || ks.del) { enterMenu(); playTone(523, 50); return; }
  if (isUp(ch))   { settingIndex = (settingIndex + SET_COUNT - 1) % SET_COUNT; resetArmed = false; playTone(700, 25); return; }
  if (isDown(ch)) { settingIndex = (settingIndex + 1) % SET_COUNT; resetArmed = false; playTone(700, 25); return; }

  int delta = isLeft(ch) || ch == '-' || ch == '[' ? -1
            : isRight(ch) || ch == '=' || ch == '+' || ch == ']' ? 1 : 0;
  bool confirm = ks.enter || ch == ' ';

  switch (settingIndex) {
    case SET_VOLUME: if (delta) changeVolume(delta, now); break;
    case SET_SOUND:  if (delta || confirm) toggleMute(now); break;
    case SET_BRIGHT: if (delta) changeBrightness(delta, now); break;
    case SET_LANG:
      if (delta) changeLanguage(delta, now);
      else if (confirm) changeLanguage(language == LANG_PT ? 1 : -1, now);
      break;
    case SET_RESET:
      if (!confirm) break;
      if (resetArmed && uint32_t(now - resetArmedAt) < RESET_CONFIRM_MS) {
        resetArmed = false;
        resetStats();
        showToast(tr(TXT_SCORE_RESET), now);
        playTone(300, 120);
      } else {
        resetArmed = true;
        resetArmedAt = now;
        playTone(500, 60);
      }
      break;
    case SET_BACK:
      if (confirm) { enterMenu(); playTone(523, 50); }
      break;
  }
}

void handleGameKey(char ch, const Keyboard_Class::KeysState& ks, uint32_t now) {
  // Volume e mudo funcionam a qualquer momento, inclusive durante o giro.
  if (ch == '[' || isLeft(ch))  { changeVolume(-1, now); return; }
  if (ch == ']' || isRight(ch)) { changeVolume(+1, now); return; }
  if (ch == 'm' || ch == 'M')   { toggleMute(now); return; }

  if (state == GAME_OVER) return;           // aguarda a recarga automatica
  if (state == CELEBRATING) {               // tecla encerra a comemoracao
    state = READY;
    melodyLength = 0;
    snprintf(message, sizeof(message), "%s", tr(TXT_READY));
    return;
  }
  if (state != READY) return;               // ignora outras teclas durante o giro

  if (isEsc(ch)) { enterMenu(); playTone(523, 50); return; }

  if (ch == '-' || ch == '_' || ch == '=' || ch == '+') {
    bool up = (ch == '=' || ch == '+');
    betIndex = constrain(betIndex + (up ? 1 : -1), 0, BET_OPTION_COUNT - 1);
    snprintf(message, sizeof(message), tr(TXT_BET_TOAST), (long)bet());
    prefs.putUChar("bet", (uint8_t)betIndex);
    playTone(up ? 990 : 660, 40);
    return;
  }
  (void)ks;
  startSpin(now);
}

void handleKey(uint32_t now) {
  Keyboard_Class::KeysState ks = M5Cardputer.Keyboard.keysState();
  char ch = ks.word.empty() ? 0 : ks.word[0];
  switch (currentScreen) {
    case SCREEN_MENU:     handleMenuKey(ch, ks, now); break;
    case SCREEN_SETTINGS: handleSettingsKey(ch, ks, now); break;
    case SCREEN_GAME:     handleGameKey(ch, ks, now); break;
  }
}

// ---------------------------------------------------------------- Desenho
void drawSymbol(M5Canvas& c, uint8_t s, int cx, int cy, uint16_t accent) {
  switch (s) {
    case SYM_CHERRY:
      c.drawLine(cx - 8, cy + 2, cx + 1, cy - 15, C_DKGREEN);
      c.drawLine(cx - 7, cy + 2, cx + 2, cy - 15, C_DKGREEN);
      c.drawLine(cx + 8, cy, cx + 1, cy - 15, C_DKGREEN);
      c.drawLine(cx + 9, cy, cx + 2, cy - 15, C_DKGREEN);
      c.fillTriangle(cx + 2, cy - 15, cx + 14, cy - 18, cx + 8, cy - 10, C_GREEN);
      c.fillCircle(cx - 8, cy + 8, 9, C_RED);
      c.fillCircle(cx + 9, cy + 6, 9, C_DKRED);
      c.fillCircle(cx - 11, cy + 5, 2, C_WHITE);
      c.fillCircle(cx + 6, cy + 3, 2, C_WHITE);
      break;
    case SYM_LEMON:
      c.fillEllipse(cx, cy, 20, 13, 0xFFE0);
      c.fillCircle(cx - 20, cy, 3, 0xFFE0);
      c.fillCircle(cx + 20, cy, 3, 0xFFE0);
      c.drawEllipse(cx, cy, 20, 13, 0xC5C0);
      c.fillEllipse(cx - 7, cy - 5, 6, 3, 0xFFF6);
      break;
    case SYM_ORANGE:
      c.fillCircle(cx, cy + 2, 17, 0xFC60);
      c.drawCircle(cx, cy + 2, 17, 0xC220);
      c.drawLine(cx, cy - 15, cx + 1, cy - 19, C_DKGREEN);
      c.fillEllipse(cx + 7, cy - 17, 6, 3, C_GREEN);
      c.fillCircle(cx - 6, cy - 4, 4, 0xFDA0);
      c.drawPixel(cx + 6, cy + 8, 0xC220);
      c.drawPixel(cx - 3, cy + 11, 0xC220);
      c.drawPixel(cx + 10, cy - 1, 0xC220);
      break;
    case SYM_GRAPE: {
      const int8_t gx[] = {-10, 0, 10, -5, 5, 0, -15, 15};
      const int8_t gy[] = {-6, -6, -6, 3, 3, 12, -10, -10};
      c.drawLine(cx, cy - 12, cx + 3, cy - 20, C_DKGREEN);
      c.fillEllipse(cx + 9, cy - 18, 6, 3, C_GREEN);
      for (int i = 5; i >= 0; --i) {
        c.fillCircle(cx + gx[i], cy + gy[i], 6, 0x780F);
        c.fillCircle(cx + gx[i] - 2, cy + gy[i] - 2, 2, 0xC39F);
      }
      break;
    }
    case SYM_BELL:
      c.fillCircle(cx, cy - 17, 3, C_GOLD);
      c.fillCircle(cx, cy - 5, 11, C_GOLD);
      c.fillRect(cx - 11, cy - 5, 22, 10, C_GOLD);
      c.fillTriangle(cx - 11, cy + 2, cx - 17, cy + 11, cx + 17, cy + 11, C_GOLD);
      c.fillTriangle(cx + 11, cy + 2, cx - 11, cy + 2, cx + 17, cy + 11, C_GOLD);
      c.fillCircle(cx, cy + 14, 4, 0xC300);
      c.fillRoundRect(cx - 5, cy - 11, 3, 13, 1, 0xFFF6);
      break;
    case SYM_SEVEN:
    default:
      c.setTextDatum(middle_center);
      c.setTextSize(4);
      c.setTextColor(C_BLACK);
      c.drawString("7", cx + 2, cy + 2);   // sombra
      c.setTextColor(accent);              // cor variavel do texto
      c.drawString("7", cx, cy);
      break;
  }
}


void drawReel(const Reel& r, int index, int y, uint32_t now, bool winner, bool flash) {
  reelCanvas.fillSprite(flash ? 0x4208 : REEL_BG);
  int cell = int(floorf(r.position));
  int offset = int((r.position - float(cell)) * CELL_H);
  for (int neighbor = -1; neighbor <= 1; ++neighbor) {
    int n = cell + neighbor;
    if (n < 0 || n >= SEQUENCE_LENGTH) continue;
    bool cycling = !r.stopped || winner;
    int colorIndex = (r.colorOffset + (cycling ? now / 90 : 0) + neighbor + COLOR_COUNT) % COLOR_COUNT;
    drawSymbol(reelCanvas, r.sequence[n], REEL_W / 2,
               REEL_H / 2 + neighbor * CELL_H - offset, COLORS[colorIndex]);
  }
  // Sombras nas bordas reforcam a ilusao de cilindro.
  reelCanvas.fillRect(0, 0, REEL_W, 2, C_BLACK);
  reelCanvas.fillRect(0, REEL_H - 2, REEL_W, 2, C_BLACK);
  reelCanvas.pushSprite(&screen, REEL_X[index], y);

  uint16_t border = winner ? COLORS[(now / 100 + index) % COLOR_COUNT]
                           : (r.stopped ? C_GOLD : C_CYAN);
  screen.drawRoundRect(REEL_X[index] - 2, y - 2, REEL_W + 4, REEL_H + 4, 5, border);
  if (winner) screen.drawRoundRect(REEL_X[index] - 3, y - 3, REEL_W + 6, REEL_H + 6, 6, border);
}

void drawTitle(const char* text, uint16_t color) {
  screen.setTextDatum(middle_center);
  screen.setTextSize(2);
  screen.setTextColor(color);
  screen.drawString(text, 120, 11);
  screen.setTextSize(1);
}

// Barra de nivel com N segmentos: usada para volume e brilho.
void drawLevelBar(int x, int y, int level, int maxLevel, uint16_t color) {
  for (int i = 0; i < maxLevel; ++i) {
    int bx = x + i * 7;
    if (i < level) screen.fillRect(bx, y, 5, 8, color);
    else screen.drawRect(bx, y, 5, 8, C_GRAY);
  }
}

void drawToast(uint32_t now) {
  if (!toast[0] || uint32_t(now - toastAt) >= TOAST_MS) return;
  int w = strlen(toast) * 6 + 16;
  int x = 120 - w / 2;
  screen.fillRoundRect(x, 58, w, 20, 5, C_BLACK);
  screen.drawRoundRect(x, 58, w, 20, 5, C_GOLD);
  screen.setTextDatum(middle_center);
  screen.setTextSize(1);
  screen.setTextColor(C_GOLD);
  screen.drawString(toast, 120, 68);
}

void drawMenu(uint32_t now) {
  screen.fillSprite(BG);
  drawTitle(tr(TXT_TITLE), COLORS[(now / 300) % COLOR_COUNT]);

  // Rolos decorativos girando devagar sem parar.
  for (int i = 0; i < REEL_COUNT; ++i) {
    Reel& r = demoReels[i];
    r.position = fmodf(now / (260.0f + i * 60.0f), float(SEQUENCE_LENGTH - 2));
    drawReel(r, i, MENU_REEL_Y, now, false, false);
  }

  const char* items[MENU_COUNT] = {tr(TXT_PLAY), tr(TXT_SETTINGS)};
  for (int i = 0; i < MENU_COUNT; ++i) {
    int y = 94 + i * 15;
    bool sel = (i == menuIndex);
    if (sel) {
      uint16_t hl = COLORS[(now / 150) % COLOR_COUNT];
      screen.fillRoundRect(52, y - 6, 136, 13, 4, hl);
      screen.setTextColor(C_BLACK);
    } else {
      screen.setTextColor(C_WHITE);
    }
    screen.setTextDatum(middle_center);
    char line[24];
    snprintf(line, sizeof(line), "%s %s", sel ? ">" : " ", items[i]);
    screen.drawString(line, 120, y);
  }

  screen.setTextColor(C_CYAN);
  char line[48];
  if ((now / INFO_ROTATE_MS) % 2) snprintf(line, sizeof(line), "%s", tr(TXT_MENU_HINT));
  else snprintf(line, sizeof(line), tr(TXT_MENU_STATS),
                (long)best, (long)credits, (unsigned long)jackpots);
  screen.drawString(line, 120, 127);
  drawToast(now);
  screen.pushSprite(0, 0);
}

void drawSettings(uint32_t now) {
  screen.fillSprite(BG);
  drawTitle(tr(TXT_SETTINGS), C_GOLD);

  const char* labels[SET_COUNT] = {tr(TXT_VOLUME), tr(TXT_SOUND), tr(TXT_BRIGHTNESS),
                                   tr(TXT_LANGUAGE), tr(TXT_RESET_SCORE), tr(TXT_BACK)};
  for (int i = 0; i < SET_COUNT; ++i) {
    int y = 23 + i * 16;
    bool sel = (i == settingIndex);
    screen.fillRoundRect(6, y, 228, 15, 4, sel ? C_PANEL : BG);
    if (sel) screen.drawRoundRect(6, y, 228, 15, 4, COLORS[(now / 150) % COLOR_COUNT]);

    screen.setTextDatum(middle_left);
    screen.setTextColor(sel ? C_GOLD : C_WHITE);
    screen.drawString(labels[i], 14, y + 8);

    char value[24] = "";
    uint16_t vcolor = C_CYAN;
    switch (i) {
      case SET_VOLUME:
        drawLevelBar(118, y + 4, volumeLevel, VOLUME_STEPS, muted ? C_GRAY : C_GREEN);
        snprintf(value, sizeof(value), "%d", volumeLevel);
        break;
      case SET_SOUND:
        snprintf(value, sizeof(value), "%s", tr(muted ? TXT_OFF : TXT_ON));
        vcolor = muted ? C_RED : C_GREEN;
        break;
      case SET_BRIGHT:
        drawLevelBar(118, y + 4, brightLevel, BRIGHT_STEPS, C_GOLD);
        snprintf(value, sizeof(value), "%d", brightLevel);
        break;
      case SET_LANG:
        snprintf(value, sizeof(value), "%s", LANGUAGE_NAMES[language]);
        break;
      case SET_RESET:
        snprintf(value, sizeof(value), "%s", tr(resetArmed ? TXT_ENTER_AGAIN : TXT_ENTER));
        vcolor = resetArmed ? C_RED : C_GRAY;
        break;
      case SET_BACK:
        snprintf(value, sizeof(value), "%s", tr(TXT_ENTER));
        vcolor = C_GRAY;
        break;
    }
    screen.setTextDatum(middle_right);
    screen.setTextColor(vcolor);
    screen.drawString(value, 228, y + 8);
  }

  screen.setTextDatum(middle_center);
  screen.setTextColor(C_CYAN);
  screen.drawString(tr(TXT_SETTINGS_HINT), 120, 127);
  drawToast(now);
  screen.pushSprite(0, 0);
}

void drawGame(uint32_t now) {
  bool celebrating = state == CELEBRATING;
  bool flash = celebrating && ENABLE_FLASH && (((now - celebrationStarted) / FLASH_PHASE_MS) & 1);
  screen.fillSprite(flash ? 0x3010 : BG);
  drawTitle(tr(TXT_TITLE), celebrating ? COLORS[(now / 120) % COLOR_COUNT] : C_GOLD);

  char line[48];
  screen.setTextDatum(top_left);
  screen.setTextColor(C_WHITE);
  snprintf(line, sizeof(line), tr(TXT_GAME_HEADER), (long)credits, (long)best, (long)bet());
  screen.drawString(line, 6, 27);
  screen.setTextDatum(top_right);
  if (muted || volumeLevel == 0) {
    screen.setTextColor(C_RED);
    screen.drawString(tr(TXT_MUTE), 234, 27);
  } else {
    screen.setTextColor(C_GREEN);
    snprintf(line, sizeof(line), "VOL%d", volumeLevel);
    screen.drawString(line, 234, 27);
  }

  for (int i = 0; i < REEL_COUNT; ++i) {
    bool winner = celebrating && (winMask & (1 << i));
    bool rflash = winner && (((now - celebrationStarted) / FLASH_PHASE_MS) & 1);
    drawReel(reels[i], i, REEL_Y, now, winner, rflash);
  }
  screen.fillTriangle(3, 65, 9, 70, 3, 75, C_GOLD);      // marcadores da linha
  screen.fillTriangle(237, 65, 231, 70, 237, 75, C_GOLD);

  screen.setTextDatum(middle_center);
  screen.setTextColor(celebrating ? COLORS[(now / 80) % COLOR_COUNT] : C_WHITE);
  screen.drawString(message, 120, 109);

  // Linha de informacao alterna entre tabela, controles e estatisticas.
  screen.setTextColor(C_CYAN);
  switch ((now / INFO_ROTATE_MS) % 3) {
    case 0: snprintf(line, sizeof(line), tr(TXT_PAY_TABLE),
                     (long)PAY_PAIR, (long)PAY_TRIPLE, (long)PAY_JACKPOT); break;
    case 1: snprintf(line, sizeof(line), "%s", tr(TXT_CONTROLS)); break;
    default: snprintf(line, sizeof(line), tr(TXT_STATS),
                      (unsigned long)spins, (unsigned long)wins, (unsigned long)jackpots); break;
  }
  screen.drawString(line, 120, 125);

  if (celebrating) {
    uint16_t border = flash ? C_GOLD : C_CYAN;
    screen.drawRect(0, 0, 240, 135, border);
    screen.drawRect(1, 1, 238, 133, border);
    // Confete apenas nas faixas laterais, sem cobrir textos nem rolos.
    for (int i = 0; i < 12; ++i) {
      int x = (i & 1) ? 3 + (i * 5) % 6 : 231 + (i * 3) % 6;
      int y = ((now - celebrationStarted) / 14 + i * 23) % 131;
      if (y > 60 && y < 80) continue;      // evita os marcadores da linha
      screen.fillRect(x, y, 3, 3, COLORS[i % COLOR_COUNT]);
    }
  }
  drawToast(now);
  screen.pushSprite(0, 0);
}

void drawGameOver(uint32_t now) {
  screen.fillSprite(BG);
  uint32_t elapsed = now - gameOverStarted;
  if (elapsed > GAME_OVER_MS) elapsed = GAME_OVER_MS;
  uint16_t accent = COLORS[(now / 150) % COLOR_COUNT];

  // Moldura piscante para destacar o fim de jogo.
  uint16_t border = ((now / 200) & 1) ? C_RED : C_GOLD;
  screen.drawRect(0, 0, 240, 135, border);
  screen.drawRect(1, 1, 238, 133, border);

  screen.setTextDatum(middle_center);
  screen.setTextSize(3);
  screen.setTextColor(C_RED);
  screen.drawString(tr(TXT_GAME_OVER), 120, 40);

  screen.setTextSize(2);
  screen.setTextColor(accent);
  screen.drawString(tr(TXT_NO_CREDITS), 120, 70);

  screen.setTextSize(1);
  screen.setTextColor(C_WHITE);
  char line[40];
  int remaining = (GAME_OVER_MS - elapsed + 999) / 1000;
  snprintf(line, sizeof(line), tr(TXT_REFILLING), remaining);
  screen.drawString(line, 120, 94);

  int barW = 180;
  int barX = (240 - barW) / 2;
  screen.drawRect(barX, 104, barW, 10, C_GOLD);
  screen.fillRect(barX + 1, 105, int(elapsed * (barW - 2) / GAME_OVER_MS), 8, C_GOLD);

  screen.setTextColor(C_CYAN);
  snprintf(line, sizeof(line), tr(TXT_REFILL_INFO), (long)START_CREDITS);
  screen.drawString(line, 120, 124);

  drawToast(now);
  screen.pushSprite(0, 0);
}

void drawScreen(uint32_t now) {
  switch (currentScreen) {
    case SCREEN_MENU:     drawMenu(now); break;
    case SCREEN_SETTINGS: drawSettings(now); break;
    case SCREEN_GAME:
      if (state == GAME_OVER) drawGameOver(now);
      else drawGame(now);
      break;
  }
}

// ---------------------------------------------------------------- Arduino
void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);             // habilita o teclado
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Mic.end();                    // microfone e alto-falante compartilham hardware
  M5Cardputer.Speaker.begin();
  randomSeed(esp_random());
  loadAll();
  applyVolume();                            // configuracoes salvas entram em vigor
  applyBrightness();

  screen.setColorDepth(16);
  reelCanvas.setColorDepth(16);
  bool screenOK = screen.createSprite(240, 135) != nullptr;
  bool reelOK = reelCanvas.createSprite(REEL_W, REEL_H) != nullptr;
  if (!screenOK || !reelOK) {
    M5Cardputer.Display.fillScreen(C_BLACK);
    M5Cardputer.Display.setTextColor(C_WHITE);
    M5Cardputer.Display.setCursor(5, 20);
    M5Cardputer.Display.print(tr(TXT_SETUP_ERROR));
    while (true) { M5Cardputer.update(); delay(10); }
  }
  screen.setTextFont(1);
  reelCanvas.setTextFont(1);

  const uint8_t initial[REEL_COUNT] = {SYM_CHERRY, SYM_BELL, SYM_SEVEN};
  for (int i = 0; i < REEL_COUNT; ++i) {    // tela inicial do jogo: cereja, sino, 7
    reels[i].finalSymbol = initial[i];
    reels[i].sequence[0] = initial[i];
    reels[i].colorOffset = i;
    Reel& d = demoReels[i];                 // sequencia ciclica para o menu
    for (int n = 0; n < SEQUENCE_LENGTH; ++n) d.sequence[n] = (n + i * 2) % SYMBOL_COUNT;
    d.colorOffset = i * 2;
    d.stopped = false;
  }
  drawScreen(millis());
}

void loop() {
  M5Cardputer.update();
  uint32_t now = millis();
  bool pressed = M5Cardputer.Keyboard.isPressed();

  // Rearma so depois de soltar todas as teclas: evita acoes repetidas.
  if (!pressed) keyboardArmed = true;
  if (pressed && keyboardArmed) {
    keyboardArmed = false;
    handleKey(now);
  }

  updateGame(now);
  updateAudio(now);
  if (uint32_t(now - lastFrame) >= FRAME_MS) {
    lastFrame = now;
    drawScreen(now);
  }
  delay(1);
}