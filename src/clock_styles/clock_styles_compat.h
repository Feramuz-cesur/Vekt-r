/*
 * clock_styles_compat.h
 *
 * Compatibility shim for clock animation code adapted from:
 *   https://github.com/Keralots/SmallOLED-PCMonitor
 *   Original author: Keralots
 *
 * The original repository ships these clock animations as part of a full
 * PlatformIO firmware that owns its own Settings/MetricData/display layer.
 * To embed the animations into this robot project without dragging in the
 * rest of that firmware, this header re-creates the symbols the clock
 * files expect:
 *   - DISPLAY_WHITE / DISPLAY_BLACK macros
 *   - extern `display` (Adafruit_SH1106G in this project)
 *   - PortedClockSettings struct + global `settings`
 *   - PortedMetricData struct + global `metricData`
 *   - `wifiConnected`, `ntpSynced` flags
 *   - getTimeWithTimeout() bridging to getLocalTime()
 *
 * The clock_*.cpp files are kept byte-identical except for include paths,
 * so the original behavior is preserved.
 */

#ifndef CLOCK_STYLES_COMPAT_H
#define CLOCK_STYLES_COMPAT_H

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <time.h>

// ========== Display abstraction (replaces ../display/display.h) ==========
extern Adafruit_SH1106G display;

#ifndef SCREEN_WIDTH
  #define SCREEN_WIDTH 128
#endif
#ifndef SCREEN_HEIGHT
  #define SCREEN_HEIGHT 64
#endif

#ifndef DISPLAY_WHITE
  #define DISPLAY_WHITE SH110X_WHITE
#endif
#ifndef DISPLAY_BLACK
  #define DISPLAY_BLACK SH110X_BLACK
#endif

// Safety cap on how long the animated clock can keep a digit "stuck" on a
// transitional value before forcibly resyncing to real time. (60 s = stock)
#ifndef TIME_OVERRIDE_MAX_MS
  #define TIME_OVERRIDE_MAX_MS 60000
#endif

// ========== Settings struct (subset used by clock animations) ==========
// Only the fields that the ported clock code actually reads. Defaults are
// taken from the original project's stock configuration.
struct PortedClockSettings {
  // Common clock options
  bool    use24Hour;
  uint8_t colonBlinkMode;   // 0=Always On, 1=Blink, 2=Always Off
  uint8_t colonBlinkRate;   // tenths of Hz
  uint8_t dateFormat;       // 0=DD/MM/YYYY

  // Mario
  uint8_t marioBounceHeight;   // tenths
  uint8_t marioBounceSpeed;    // tenths
  bool    marioSmoothAnimation;
  uint8_t marioWalkSpeed;      // tenths
  bool    marioIdleEncounters;
  uint8_t marioEncounterFreq;
  uint8_t marioEncounterSpeed;

  // Space
  uint8_t spaceCharacterType;   // 0=Invader, 1=Ship
  uint8_t spacePatrolSpeed;
  uint8_t spaceAttackSpeed;
  uint8_t spaceLaserSpeed;
  uint8_t spaceExplosionGravity;

  // Pong
  uint8_t pongBallSpeed;
  uint8_t pongBounceDamping;
  uint8_t pongBounceStrength;
  bool    pongHorizontalBounce;
  uint8_t pongPaddleWidth;

  // Pac-Man
  uint8_t pacmanSpeed;
  uint8_t pacmanEatingSpeed;
  uint8_t pacmanMouthSpeed;
  uint8_t pacmanPelletCount;
  bool    pacmanPelletRandomSpacing;
  bool    pacmanBounceEnabled;
};
extern PortedClockSettings settings;

// ========== Metric data mock (clock code only reads `online`) ==========
struct PortedMetricData {
  bool online;
};
extern PortedMetricData metricData;

// ========== Status flags expected by clock code ==========
extern bool wifiConnected;
extern bool ntpSynced;

// ========== Time helper (replaces getTimeWithTimeout in main.cpp) ==========
// Default argument is declared in clocks.h (forwarded from the original
// API). Re-specifying it here would violate C++'s "default arguments shall
// not appear in a redeclaration" rule, so this declaration is bare.
bool getTimeWithTimeout(struct tm* timeinfo, unsigned long timeout_ms);

// ========== Mario Clock Types ==========
enum MarioState {
  MARIO_IDLE,
  MARIO_WALKING,
  MARIO_JUMPING,
  MARIO_WALKING_OFF,
  MARIO_ENCOUNTER_WALKING,
  MARIO_ENCOUNTER_JUMPING,
  MARIO_ENCOUNTER_SHOOTING,
  MARIO_ENCOUNTER_SQUASH,
  MARIO_ENCOUNTER_RETURNING
};

enum EnemyType  { ENEMY_NONE, ENEMY_GOOMBA, ENEMY_SPINY, ENEMY_KOOPA };
enum EnemyState { ENEMY_WALKING, ENEMY_SQUASHING, ENEMY_HIT, ENEMY_DEAD, ENEMY_SHELL_SLIDING };

struct MarioEnemy {
  EnemyType  type;
  EnemyState state;
  float x;
  int walkFrame;
  uint8_t animTimer;
  bool fromRight;
};

struct MarioFireball {
  float x, y;
  float vy;
  bool active;
};

// Mario animation constants
#define MARIO_ANIM_SPEED 35
#define ENCOUNTER_ANIM_SPEED 16
#define ENCOUNTER_TIME_SCALE (ENCOUNTER_ANIM_SPEED / (float)MARIO_ANIM_SPEED)
#define JUMP_POWER -4.5
#define GRAVITY 0.6
#define TIME_Y 26
#define MARIO_HEAD_OFFSET 10
#define DIGIT_BOTTOM (TIME_Y + 21)

// Shared digit X positions for time display
extern const int DIGIT_X[5];

// ========== Space Clock Types ==========
enum SpaceState {
  SPACE_PATROL,
  SPACE_SLIDING,
  SPACE_SHOOTING,
  SPACE_EXPLODING_DIGIT,
  SPACE_MOVING_NEXT,
  SPACE_RETURNING
};

struct Laser {
  float x, y;
  float length;
  bool active;
  int target_digit_idx;
};

#define MAX_SPACE_FRAGMENTS 20
#define LASER_MAX_LENGTH 50
#define SPACE_PATROL_LEFT 20
#define SPACE_PATROL_RIGHT 108

struct SpaceFragment {
  float x, y;
  float vx, vy;
  bool active;
};

// ========== Pong Clock Types ==========
enum PongBallState {
  PONG_BALL_NORMAL,
  PONG_BALL_SPAWNING
};

enum DigitTransitionState {
  DIGIT_NORMAL,
  DIGIT_BREAKING,
  DIGIT_ASSEMBLING
};

struct PongBall {
  int x, y;
  int vx, vy;
  PongBallState state;
  unsigned long spawn_timer;
  bool active;
  int inside_digit;
};

struct DigitTransition {
  DigitTransitionState state;
  char old_char;
  char new_char;
  unsigned long state_timer;
  int hit_count;
  int fragments_spawned;
  float assembly_progress;
};

struct BreakoutPaddle {
  int x;
  int target_x;
  int width;
  int speed;
};

struct FragmentTarget {
  int target_digit;
  int target_x;
  int target_y;
};

// Pong constants
#define MAX_PONG_BALLS 2
#define MAX_PONG_FRAGMENTS 40
#define PONG_BALL_SIZE 2
#define PONG_TIME_Y 16
#define PONG_PLAY_AREA_TOP 10
#define BREAKOUT_PADDLE_Y 60
#define BREAKOUT_PADDLE_HEIGHT 2
#define PONG_UPDATE_INTERVAL 20
#define BALL_SPAWN_DELAY 500
#define PONG_FRAG_GRAVITY 0.3
#define PONG_FRAG_SPEED 1.5
#define BALL_HIT_THRESHOLD 3
#define DIGIT_TRANSITION_TIMEOUT 3000
#define DIGIT_ASSEMBLY_DURATION 800
#define PONG_BALL_SPEED_BOOST 28
#define MULTIBALL_ACTIVATE_SECOND 55
#define PADDLE_WRONG_DIRECTION_CHANCE 0
#define PADDLE_STICK_MIN_DELAY 0
#define PADDLE_STICK_MAX_DELAY 300
#define PADDLE_MOMENTUM_MULTIPLIER 2
#define BALL_RELEASE_RANDOM_VARIATION 2
#define BALL_COLLISION_ANGLE_VARIATION 3

extern const float FRAGMENT_SPAWN_PERCENT[3];

// ========== Pac-Man Clock Types ==========
enum PacmanState {
  PACMAN_PATROL,
  PACMAN_TARGETING,
  PACMAN_EATING,
  PACMAN_RETURNING
};

struct PatrolPellet {
  int x;
  bool active;
};

struct PathStep {
  uint8_t col;
  uint8_t row;
};

// Pac-Man constants
#define PACMAN_ANIM_SPEED 30
#define PACMAN_PATROL_Y 56
#define MAX_PATROL_PELLETS 20
#define TIME_Y_PACMAN 16
#define PELLET_SPACING 5
#define PELLET_SIZE 1
#define DIGIT_GRID_W 5
#define DIGIT_GRID_H 7

#endif // CLOCK_STYLES_COMPAT_H
