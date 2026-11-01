#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
typedef void (*PFNGLCLEARPROC)(unsigned int mask);
extern PFNGLCLEARPROC glad_glClear;
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void SendEvent(int event_id, int event_value) {
  int msg_type = 1;
  fwrite(&msg_type, sizeof(int), 1, stdout);
  fwrite(&event_id, sizeof(int), 1, stdout);
  fwrite(&event_value, sizeof(int), 1, stdout);
  fflush(stdout);
}

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
#include <stdarg.h>

void CustomLog(int logLevel, const char *text, va_list args) {
  vfprintf(stderr, text, args);
  fprintf(stderr, "\n");
  fflush(stderr);
}

#define MAX_ENEMIES 128
#define MAX_SPRITES 256
#define MAX_PROJECTILES 128

typedef struct {
  float x, y, z;
  int state;
  int tex_idx;
  int type;
  float move_speed;
  float timer;
  float hp;
  float max_hp;
  float move_timer;
  int anim_frame;
  int burst_count;
  float reload_timer;
  float dodge_timer;
  float attack_range;
  float damage;
} Enemy;

typedef struct {
  float x, y, z;
  int type;
  int state;
} StaticSprite;

typedef struct {
  float x, z;
} SpawnPoint;

typedef struct {
  Vector3 pos;
  Vector3 dir;
  float speed;
  float damage;
  int active;
  int tex_idx; // 6 = yellow bullet, 14 = red bullet
  int from_player; // 1 = player, 0 = enemy
} Projectile;

Enemy enemies[MAX_ENEMIES];
StaticSprite static_sprites[MAX_SPRITES];
SpawnPoint spawn_points[64];
Projectile projectiles[MAX_PROJECTILES];

int num_enemies = 0;
int num_static_sprites = 0;
int num_spawn_points = 0;

int map_width = 0;
int map_height = 0;
int map_layers = 0;
int *dynamic_map = NULL;
int is_arena_mode = 0;

int GetBlock(int mapX, int mapY, int mapZ) {
  if (mapX < 0 || mapX >= map_width || mapZ < 0 || mapZ >= map_height)
    return 1;
  if (mapY < 0 || mapY >= map_layers)
    return 0;
  return dynamic_map[(mapY * map_height * map_width) + (mapZ * map_width) + mapX];
}

int HasLineOfSight(Vector3 from, Vector3 to) {
  Vector3 delta = Vector3Subtract(to, from);
  float dist = Vector3Length(delta);
  if (dist < 0.2f) return 1;
  Vector3 dir = Vector3Scale(delta, 1.0f / dist);
  float step = 0.5f;
  float t = 0.5f;
  while (t < dist - 0.5f) {
    Vector3 p = Vector3Add(from, Vector3Scale(dir, t));
    int bx = (int)roundf(p.x / 2.0f);
    int bz = (int)roundf(p.z / 2.0f);
    int layer = (int)floorf(p.y / 2.0f);
    if (GetBlock(bx, layer, bz) > 0) return 0;
    t += step;
  }
  return 1;
}

void SpawnProjectile(Vector3 pos, Vector3 dir, float speed, float damage, int tex_idx, int from_player) {
  for (int i = 0; i < MAX_PROJECTILES; i++) {
    if (!projectiles[i].active) {
      projectiles[i].pos = pos;
      projectiles[i].dir = Vector3Normalize(dir);
      projectiles[i].speed = speed;
      projectiles[i].damage = damage;
      projectiles[i].tex_idx = tex_idx;
      projectiles[i].from_player = from_player;
      projectiles[i].active = 1;
      return;
    }
  }
}

Texture2D LoadFlippedTexture(const char *path) {
  Image img = LoadImage(path);
  ImageFlipVertical(&img);
  Texture2D tex = LoadTextureFromImage(img);
  UnloadImage(img);
  if (tex.id > 1) {
    GenTextureMipmaps(&tex);
    SetTextureFilter(tex, TEXTURE_FILTER_ANISOTROPIC_8X);
  }
  return tex;
}

typedef struct {
  int w, s, a, d;
  int left, right, up, down;
  int space, shift, ctrl, shoot;
  float mouse_dx, mouse_dy;
  float time_of_day;
  int lighting_quality;
  int shadows_enabled;
  int active_weapon;
  int bloom_enabled;
  int volumetric_clouds;
  float rain_intensity;
  int soft_shadows;
  int is_aiming;
  int pistol_lvl;
  int shotgun_lvl;
  int minigun_lvl;
  int flashlight_active;
  float wp_hip_x, wp_hip_y, wp_hip_z;
  float wp_ads_x, wp_ads_y, wp_ads_z;
  float wp_pitch, wp_yaw, wp_roll;
  float wp_scale;
  float motion_blur_strength;
} InputPacket;

typedef struct {
  float x;
  float y;
  float z;
  float pitch;
  float yaw;
  float time_of_day;
  int lighting_quality;
  int shadows_enabled;
  float vel_y;
  int is_grounded;
} CameraData;

static CameraData current_cam = {2.0f,  1.0f, 2.0f, 0.0f, 0.0f,
                                 12.0f, 0,    0,    0.0f, 0};

typedef struct {
    float cooldown;
    float damage;
    int pellets;
    float spread;
    float recoil_z;
    float recoil_pitch;
} WeaponDef;

WeaponDef weapons[4] = {
    { 0.5f,  25.0f, 1, 0.00f, 0.01f, 5.0f },   // 0: fist/sword
    { 0.38f, 50.0f, 1, 0.00f, 0.05f, 15.0f },  // 1: M1911A1
    { 1.4f,  35.0f, 5, 0.15f, 0.12f, 30.0f },  // 2: shotgun
    { 0.05f, 40.0f, 1, 0.05f, 0.02f, 5.0f }    // 3: minigun
};

typedef struct {
    Vector3 hip_pos;
    Vector3 ads_pos;
    float pitch, yaw, roll;
    float scale;
} WeaponTransform;

WeaponTransform default_weapon_transforms[4] = {
    { {0.055f, -0.019f, -0.1f}, {0.036f, -0.014f, -0.11f}, 0.0f, 180.0f, 0.0f, 0.301f },  // 0: fist/sword
    { {0.078f, -0.116f, -0.298f}, {0.0f, -0.0612f, -0.312f}, 0.0f, 0.0f, 0.0f, 1.0f },     // 1: M1911A1
    { {0.048f, -0.039f, -0.1f}, {0.0f, -0.0236f, -0.07f}, 0.0f, 180.0f, 0.0f, 0.775f },  // 2: shotgun
    { {0.058f, -0.085f, -0.1f}, {0.0f, -0.085f, -0.073f}, 0.0f, 180.0f, 0.0f, 0.521f }   // 3: minigun
};

typedef struct {
    float pos;
    float vel;
    float target;
} SpringDamper1D;

static inline void UpdateSpringDamper(SpringDamper1D *s, float stiffness, float damping, float dt) {
    if (dt > 0.05f) dt = 0.05f;
    float force = -stiffness * (s->pos - s->target) - damping * s->vel;
    s->vel += force * dt;
    s->pos += s->vel * dt;
}

static inline Matrix MatrixFromBasisOrigin(Vector3 x_axis, Vector3 y_axis, Vector3 z_axis, Vector3 origin) {
    Matrix m = { 0 };
    m.m0 = x_axis.x; m.m4 = y_axis.x; m.m8  = z_axis.x; m.m12 = origin.x;
    m.m1 = x_axis.y; m.m5 = y_axis.y; m.m9  = z_axis.y; m.m13 = origin.y;
    m.m2 = x_axis.z; m.m6 = y_axis.z; m.m10 = z_axis.z; m.m14 = origin.z;
    m.m3 = 0.0f;     m.m7 = 0.0f;     m.m11 = 0.0f;     m.m15 = 1.0f;
    return m;
}

static inline Matrix MatrixInvertRigid(Matrix m) {
    Matrix inv = { 0 };
    inv.m0 = m.m0; inv.m4 = m.m1; inv.m8  = m.m2;
    inv.m1 = m.m4; inv.m5 = m.m5; inv.m9  = m.m6;
    inv.m2 = m.m8; inv.m6 = m.m9; inv.m10 = m.m10;
    float tx = m.m12, ty = m.m13, tz = m.m14;
    inv.m12 = -(inv.m0 * tx + inv.m4 * ty + inv.m8  * tz);
    inv.m13 = -(inv.m1 * tx + inv.m5 * ty + inv.m9  * tz);
    inv.m14 = -(inv.m2 * tx + inv.m6 * ty + inv.m10 * tz);
    inv.m15 = 1.0f;
    return inv;
}

static inline Vector3 TransformDirByMatrix(Vector3 v, Matrix m) {
    return (Vector3){
        m.m0 * v.x + m.m4 * v.y + m.m8  * v.z,
        m.m1 * v.x + m.m5 * v.y + m.m9  * v.z,
        m.m2 * v.x + m.m6 * v.y + m.m10 * v.z
    };
}

static inline Quaternion QuaternionFromBasisMapping(Vector3 b0, Vector3 b1, Vector3 b2,
                                                    Vector3 t0, Vector3 t1, Vector3 t2) {
    Matrix r = { 0 };
    r.m0  = t0.x * b0.x + t1.x * b1.x + t2.x * b2.x;
    r.m4  = t0.x * b0.y + t1.x * b1.y + t2.x * b2.y;
    r.m8  = t0.x * b0.z + t1.x * b1.z + t2.x * b2.z;

    r.m1  = t0.y * b0.x + t1.y * b1.x + t2.y * b2.x;
    r.m5  = t0.y * b0.y + t1.y * b1.y + t2.y * b2.y;
    r.m9  = t0.y * b0.z + t1.y * b1.z + t2.y * b2.z;

    r.m2  = t0.z * b0.x + t1.z * b1.x + t2.z * b2.x;
    r.m6  = t0.z * b0.y + t1.z * b1.y + t2.z * b2.y;
    r.m10 = t0.z * b0.z + t1.z * b1.z + t2.z * b2.z;
    r.m15 = 1.0f;
    return QuaternionNormalize(QuaternionFromMatrix(r));
}

typedef struct {
    int sh, sh_c;
    int arm, arm_tw1, arm_tw, arm_tw3;
    int elbow, hand_tw1, hand_tw2, hand_tw, hand_tw3;
    int wrist;
} SayoriArmChainIdx;

static inline float ChestEllipsoidValue(Vector3 p, Vector3 chest_center, Quaternion q_thorax) {
    Vector3 rel = Vector3Subtract(p, chest_center);
    Vector3 local = Vector3RotateByQuaternion(rel, QuaternionInvert(q_thorax));
    const Vector3 r = { 0.155f, 0.235f, 0.145f };
    float nx = local.x / r.x;
    float ny = local.y / r.y;
    float nz = local.z / r.z;
    return nx * nx + ny * ny + nz * nz;
}

static float ChestSwivelClearance(Vector3 S, Vector3 u_sw, Vector3 W, float L1, float cos_a, float sin_a,
                                  Vector3 bend_dir, float theta, Vector3 chest_center, Quaternion q_thorax) {
    Vector3 b = (theta == 0.0f) ? bend_dir : Vector3RotateByAxisAngle(bend_dir, u_sw, theta);
    Vector3 E = Vector3Add(S, Vector3Add(Vector3Scale(u_sw, L1 * cos_a), Vector3Scale(b, L1 * sin_a)));
    Vector3 M1 = Vector3Lerp(E, W, 0.40f);
    return fminf(ChestEllipsoidValue(E, chest_center, q_thorax),
                 ChestEllipsoidValue(M1, chest_center, q_thorax));
}

static void SolveSayoriWorldArmIK(ModelAnimation *anim, Model sayoriModel,
                                  Matrix invBodyMat, Matrix gunWorldMat, Matrix gunAnchor,
                                  const SayoriArmChainIdx *ch, bool is_right,
                                  float wall_lift_t, bool is_crouching, float look_pitch_deg,
                                  Vector3 arm_posed_base, Vector3 sh_posed_base,
                                  Quaternion spine_accum_rot, Vector3 chest_center,
                                  float shear_z, float shear_yaw,
                                  float jog_blend, float jog_gait_cycle) {
    if (!anim || !anim->keyframePoses || !anim->keyframePoses[0]) return;
    if (ch->sh < 0 || ch->arm < 0 || ch->elbow < 0 || ch->wrist < 0) return;

    float look_dn_t = Clamp((-look_pitch_deg - 12.0f) / 58.0f, 0.0f, 1.0f);
    look_dn_t = look_dn_t * look_dn_t * (3.0f - 2.0f * look_dn_t);
    float look_up_t = Clamp((look_pitch_deg - 18.0f) / 62.0f, 0.0f, 1.0f);
    look_up_t = look_up_t * look_up_t * (3.0f - 2.0f * look_up_t);

    Matrix gunToModel = MatrixMultiply(gunWorldMat, invBodyMat);

    Vector3 sh_bind       = sh_posed_base;
    Vector3 arm_bind_rest = sayoriModel.skeleton.bindPose[ch->arm].translation;
    Vector3 elbow_bind    = sayoriModel.skeleton.bindPose[ch->elbow].translation;
    Vector3 wrist_bind    = sayoriModel.skeleton.bindPose[ch->wrist].translation;
    Vector3 sh_bind_rest  = sayoriModel.skeleton.bindPose[ch->sh].translation;

    float L1 = Vector3Distance(arm_bind_rest, elbow_bind);
    float L2 = Vector3Distance(elbow_bind, wrist_bind);

    Vector3 F_bind = Vector3Normalize(Vector3Subtract(wrist_bind, elbow_bind));
    Vector3 T_raw_bind = { 0.0f, 0.0f, 1.0f };
    Vector3 T_bind = Vector3Normalize(Vector3Subtract(T_raw_bind, Vector3Scale(F_bind, Vector3DotProduct(T_raw_bind, F_bind))));

    Quaternion q_wrist_delta;
    Vector3 palm_offset_bind;
    Vector3 P_bind_ref;
    Vector3 wrist_target;
    Vector3 S;
    Vector3 pole;

    if (is_right) {
        Vector3 P_bind = Vector3CrossProduct(T_bind, F_bind);
        P_bind_ref = P_bind;
        palm_offset_bind = Vector3Add(
            Vector3Add(Vector3Scale(F_bind, 0.056f), Vector3Scale(T_bind, 0.008f)),
            Vector3Scale(P_bind, 0.014f)
        );

        Vector3 F_gun = Vector3Normalize((Vector3){ -0.22f, -0.17f, -0.96f });
        Vector3 P_raw = Vector3Normalize((Vector3){ -0.96f, +0.06f, +0.22f });
        Vector3 P_gun = Vector3Normalize(Vector3Subtract(P_raw, Vector3Scale(F_gun, Vector3DotProduct(P_raw, F_gun))));
        Vector3 T_gun = Vector3CrossProduct(F_gun, P_gun);

        Vector3 T_mod = Vector3Normalize(TransformDirByMatrix(T_gun, gunToModel));
        Vector3 F_mod = Vector3Normalize(TransformDirByMatrix(F_gun, gunToModel));
        Vector3 P_mod = Vector3CrossProduct(T_mod, F_mod);

        q_wrist_delta = QuaternionFromBasisMapping(T_bind, F_bind, P_bind, T_mod, F_mod, P_mod);

        Vector3 socket_gun = { +0.015f, -0.022f, +0.008f };
        Vector3 socket_mod = Vector3Transform(socket_gun, gunToModel);
        wrist_target = Vector3Subtract(socket_mod, Vector3RotateByQuaternion(palm_offset_bind, q_wrist_delta));

        Vector3 reach_vec = Vector3Subtract(wrist_target, arm_posed_base);
        Vector3 reach_chest = Vector3RotateByQuaternion(reach_vec, QuaternionInvert(spine_accum_rot));
        float elev_ang = Clamp((reach_chest.y - 0.02f) * 1.15f, -0.18f, 0.36f);
        float prot_ang = Clamp((reach_chest.z - 0.12f) * 1.10f,  0.00f, 0.38f);
        Quaternion q_clav_local = QuaternionMultiply(
            QuaternionFromAxisAngle((Vector3){0, 1, 0}, -prot_ang),
            QuaternionFromAxisAngle((Vector3){0, 0, 1}, -elev_ang)
        );
        Quaternion q_clav = QuaternionMultiply(spine_accum_rot, q_clav_local);
        Vector3 clav_bind_offset = Vector3Subtract(arm_bind_rest, sh_bind_rest);
        S = Vector3Add(sh_bind, Vector3RotateByQuaternion(clav_bind_offset, q_clav));
        anim->keyframePoses[0][ch->sh].translation = sh_bind;
        anim->keyframePoses[0][ch->sh].rotation = QuaternionMultiply(q_clav, sayoriModel.skeleton.bindPose[ch->sh].rotation);
        if (ch->sh_c >= 0) {
            anim->keyframePoses[0][ch->sh_c].translation = S;
            anim->keyframePoses[0][ch->sh_c].rotation = QuaternionMultiply(q_clav, sayoriModel.skeleton.bindPose[ch->sh_c].rotation);
        }

        float tuck = (is_crouching ? 0.12f : 0.0f) + wall_lift_t * 0.22f;
        Vector3 local_pole = Vector3Normalize((Vector3){
            -0.44f - look_up_t * 0.20f - look_dn_t * 0.16f + tuck,
            -0.82f - wall_lift_t * 0.20f,
            Lerp(-0.36f, +0.30f, look_dn_t) + look_up_t * 0.12f
        });
        pole = Vector3RotateByQuaternion(local_pole, spine_accum_rot);
    } else {
        Vector3 P_bind = Vector3CrossProduct(F_bind, T_bind);
        P_bind_ref = P_bind;
        palm_offset_bind = Vector3Add(
            Vector3Add(Vector3Scale(F_bind, 0.058f), Vector3Scale(T_bind, 0.008f)),
            Vector3Scale(P_bind, 0.014f)
        );

        Vector3 F_gun = Vector3Normalize((Vector3){ +0.28f, -0.14f, -0.95f });
        Vector3 P_raw = Vector3Normalize((Vector3){ +0.95f, +0.08f, +0.28f });
        Vector3 P_gun = Vector3Normalize(Vector3Subtract(P_raw, Vector3Scale(F_gun, Vector3DotProduct(P_raw, F_gun))));
        Vector3 T_gun = Vector3CrossProduct(P_gun, F_gun);

        Vector3 F_mod = Vector3Normalize(TransformDirByMatrix(F_gun, gunToModel));
        Vector3 T_mod = Vector3Normalize(TransformDirByMatrix(T_gun, gunToModel));
        Vector3 P_mod = Vector3CrossProduct(F_mod, T_mod);

        q_wrist_delta = QuaternionFromBasisMapping(F_bind, T_bind, P_bind, F_mod, T_mod, P_mod);

        if (fabsf(shear_yaw) > 0.001f) {
            Quaternion q_shear = QuaternionFromAxisAngle((Vector3){ 0.0f, 1.0f, 0.0f }, shear_yaw * DEG2RAD);
            q_wrist_delta = QuaternionMultiply(q_shear, q_wrist_delta);
        }

        Vector3 socket_gun = { -0.016f + shear_z * 0.18f, -0.026f, -0.002f - shear_z };
        Vector3 socket_mod = Vector3Transform(socket_gun, gunToModel);
        Vector3 wrist_target_gun = Vector3Subtract(socket_mod, Vector3RotateByQuaternion(palm_offset_bind, q_wrist_delta));
        wrist_target = wrist_target_gun;

        if (jog_blend > 0.001f) {
            float jog_fwd = sinf(jog_gait_cycle);
            float jog_up  = cosf(jog_gait_cycle * 2.0f);
            Vector3 jog_hand_anchor = {
                -0.095f + 0.015f * jog_fwd,
                -0.135f + 0.035f * jog_fwd + 0.012f * jog_up,
                -0.280f - 0.060f * jog_fwd
            };
            Vector3 jog_hand_world = Vector3Transform(jog_hand_anchor, gunAnchor);
            Vector3 wrist_target_jog = Vector3Transform(jog_hand_world, invBodyMat);
            wrist_target = Vector3Lerp(wrist_target_gun, wrist_target_jog, jog_blend);
        }

        Vector3 reach_vec = Vector3Subtract(wrist_target, arm_posed_base);
        Vector3 reach_chest = Vector3RotateByQuaternion(reach_vec, QuaternionInvert(spine_accum_rot));
        float elev_ang = Clamp((reach_chest.y - 0.02f) * 1.15f, -0.18f, 0.36f);
        float prot_ang = Clamp((-reach_chest.z - 0.10f) * 1.25f,  0.00f, 0.42f);
        Quaternion q_clav_local = QuaternionMultiply(
            QuaternionFromAxisAngle((Vector3){0, 1, 0}, -prot_ang),
            QuaternionFromAxisAngle((Vector3){0, 0, 1}, +elev_ang)
        );
        Quaternion q_clav = QuaternionMultiply(spine_accum_rot, q_clav_local);
        Vector3 clav_bind_offset = Vector3Subtract(arm_bind_rest, sh_bind_rest);
        S = Vector3Add(sh_bind, Vector3RotateByQuaternion(clav_bind_offset, q_clav));
        anim->keyframePoses[0][ch->sh].translation = sh_bind;
        anim->keyframePoses[0][ch->sh].rotation = QuaternionMultiply(q_clav, sayoriModel.skeleton.bindPose[ch->sh].rotation);
        if (ch->sh_c >= 0) {
            anim->keyframePoses[0][ch->sh_c].translation = S;
            anim->keyframePoses[0][ch->sh_c].rotation = QuaternionMultiply(q_clav, sayoriModel.skeleton.bindPose[ch->sh_c].rotation);
        }

        float tuck = (is_crouching ? 0.12f : 0.0f) + wall_lift_t * 0.22f;
        Vector3 local_pole_gun = Vector3Normalize((Vector3){
            +0.36f + look_up_t * 0.18f + look_dn_t * 0.16f - tuck,
            -0.86f - wall_lift_t * 0.20f,
            Lerp(-0.34f, +0.34f, look_dn_t) + look_up_t * 0.12f
        });
        Vector3 local_pole_jog = Vector3Normalize((Vector3){ +0.38f, -0.32f, -0.75f });
        Vector3 local_pole = Vector3Lerp(local_pole_gun, local_pole_jog, jog_blend);
        pole = Vector3RotateByQuaternion(local_pole, spine_accum_rot);
    }

    Vector3 sw = Vector3Subtract(wrist_target, S);
    float d = Vector3Length(sw);
    float max_d = (L1 + L2) * 0.992f;
    float min_d = fabsf(L1 - L2) + 0.035f;
    Vector3 u_sw = (d > 1e-5f) ? Vector3Scale(sw, 1.0f / d) : (Vector3){0, 0, 1};
    float d_clamped = Clamp(d, min_d, max_d);
    Vector3 W = Vector3Add(S, Vector3Scale(u_sw, d_clamped));

    float cos_a = Clamp((L1 * L1 + d_clamped * d_clamped - L2 * L2) / (2.0f * L1 * d_clamped), -1.0f, 1.0f);
    float sin_a = sqrtf(fmaxf(0.0f, 1.0f - cos_a * cos_a));

    Vector3 bend_dir = Vector3Subtract(pole, Vector3Scale(u_sw, Vector3DotProduct(pole, u_sw)));
    if (Vector3LengthSqr(bend_dir) < 1e-6f) bend_dir = (Vector3){ 0.0f, -1.0f, 0.0f };
    bend_dir = Vector3Normalize(bend_dir);

    float base_clr = ChestSwivelClearance(S, u_sw, W, L1, cos_a, sin_a, bend_dir, 0.0f, chest_center, spine_accum_rot);
    if (base_clr < 1.0f) {
        float sgn = is_right ? -1.0f : +1.0f;
        const float step = 3.0f * DEG2RAD;
        float best_theta = 0.0f;
        float best_val = base_clr;
        bool found = false;
        for (int k = 1; k <= 14 && !found; k++) {
            float th = k * step;
            float val = ChestSwivelClearance(S, u_sw, W, L1, cos_a, sin_a, bend_dir, sgn * th, chest_center, spine_accum_rot);
            if (val > best_val) {
                best_val = val;
                best_theta = sgn * th;
            }
            if (val >= 1.0f) {
                float lo = (k - 1) * step, hi = th;
                for (int it = 0; it < 6; it++) {
                    float mid = 0.5f * (lo + hi);
                    float vm = ChestSwivelClearance(S, u_sw, W, L1, cos_a, sin_a, bend_dir, sgn * mid, chest_center, spine_accum_rot);
                    if (vm >= 1.0f) hi = mid; else lo = mid;
                }
                best_theta = sgn * hi;
                found = true;
            }
        }
        bend_dir = Vector3Normalize(Vector3RotateByAxisAngle(bend_dir, u_sw, best_theta));
    }

    Vector3 E = Vector3Add(S, Vector3Add(Vector3Scale(u_sw, L1 * cos_a), Vector3Scale(bend_dir, L1 * sin_a)));

    Vector3 u_up_bind = Vector3Normalize(Vector3Subtract(elbow_bind, arm_bind_rest));
    Vector3 u_up_cur  = Vector3Normalize(Vector3Subtract(E, S));
    Quaternion q_up   = QuaternionFromVector3ToVector3(u_up_bind, u_up_cur);

    anim->keyframePoses[0][ch->arm].translation = S;
    anim->keyframePoses[0][ch->arm].rotation = QuaternionMultiply(q_up, sayoriModel.skeleton.bindPose[ch->arm].rotation);

    int up_tw_bones[3] = { ch->arm_tw1, ch->arm_tw, ch->arm_tw3 };
    for (int i = 0; i < 3; i++) {
        int tb = up_tw_bones[i];
        if (tb < 0) continue;
        Vector3 off = Vector3Subtract(sayoriModel.skeleton.bindPose[tb].translation, arm_bind_rest);
        anim->keyframePoses[0][tb].translation = Vector3Add(S, Vector3RotateByQuaternion(off, q_up));
        anim->keyframePoses[0][tb].rotation = QuaternionMultiply(q_up, sayoriModel.skeleton.bindPose[tb].rotation);
    }

    Vector3 u_fore_cur = Vector3Normalize(Vector3Subtract(W, E));
    Quaternion q_fore_swing = QuaternionFromVector3ToVector3(F_bind, u_fore_cur);

    if (!is_right && jog_blend > 0.001f) {
        Matrix anchorToModel = MatrixMultiply(gunAnchor, invBodyMat);
        Vector3 F_jog_anc = Vector3Normalize((Vector3){ +0.10f, +0.18f, -0.98f });
        Vector3 P_jog_anc = Vector3Normalize((Vector3){ +0.98f, +0.02f, +0.18f });
        Vector3 T_jog_anc = Vector3CrossProduct(P_jog_anc, F_jog_anc);
        Vector3 F_jog_mod = Vector3Normalize(TransformDirByMatrix(F_jog_anc, anchorToModel));
        Vector3 T_jog_mod = Vector3Normalize(TransformDirByMatrix(T_jog_anc, anchorToModel));
        Vector3 P_jog_mod = Vector3CrossProduct(F_jog_mod, T_jog_mod);
        Quaternion q_wrist_jog = QuaternionFromBasisMapping(F_bind, T_bind, P_bind_ref, F_jog_mod, T_jog_mod, P_jog_mod);
        q_wrist_delta = QuaternionSlerp(q_wrist_delta, q_wrist_jog, jog_blend);
    }

    Vector3 P_swing = Vector3RotateByQuaternion(P_bind_ref, q_fore_swing);
    Vector3 P_wrist = Vector3RotateByQuaternion(P_bind_ref, q_wrist_delta);
    Vector3 P_wrist_perp = Vector3Subtract(P_wrist, Vector3Scale(u_fore_cur, Vector3DotProduct(P_wrist, u_fore_cur)));
    float twist_ang = 0.0f;
    if (Vector3LengthSqr(P_wrist_perp) > 1e-6f) {
        P_wrist_perp = Vector3Normalize(P_wrist_perp);
        twist_ang = atan2f(Vector3DotProduct(Vector3CrossProduct(P_swing, P_wrist_perp), u_fore_cur),
                           Clamp(Vector3DotProduct(P_swing, P_wrist_perp), -1.0f, 1.0f));
        twist_ang = Clamp(twist_ang, -1.85f, 1.85f);
    }

    anim->keyframePoses[0][ch->elbow].translation = E;
    anim->keyframePoses[0][ch->elbow].rotation = QuaternionMultiply(q_fore_swing, sayoriModel.skeleton.bindPose[ch->elbow].rotation);

    int fore_tw_bones[4] = { ch->hand_tw1, ch->hand_tw2, ch->hand_tw, ch->hand_tw3 };
    float fore_fracs[4]  = { 0.30f, 0.52f, 0.65f, 0.82f };
    for (int i = 0; i < 4; i++) {
        int tb = fore_tw_bones[i];
        if (tb < 0) continue;
        Quaternion q_seg = QuaternionMultiply(QuaternionFromAxisAngle(u_fore_cur, twist_ang * fore_fracs[i]), q_fore_swing);
        Vector3 off = Vector3Subtract(sayoriModel.skeleton.bindPose[tb].translation, elbow_bind);
        anim->keyframePoses[0][tb].translation = Vector3Add(E, Vector3RotateByQuaternion(off, q_fore_swing));
        anim->keyframePoses[0][tb].rotation = QuaternionMultiply(q_seg, sayoriModel.skeleton.bindPose[tb].rotation);
    }

    anim->keyframePoses[0][ch->wrist].translation = W;
    anim->keyframePoses[0][ch->wrist].rotation = QuaternionMultiply(q_wrist_delta, sayoriModel.skeleton.bindPose[ch->wrist].rotation);
}

typedef struct {
    int thumb[3];
    int index[3];
    int middle[3];
    int ring[3];
    int little[3];
} SayoriHandFingers;

static void ApplyFingerChain(ModelAnimation *anim, const Model *sayori,
                             int wrist_b, const int chain[3],
                             Quaternion q_curl0, Quaternion q_curl1, Quaternion q_curl2) {
    if (!anim || !anim->keyframePoses || !anim->keyframePoses[0]) return;
    if (chain[0] < 0 || chain[1] < 0 || chain[2] < 0 || wrist_b < 0) return;

    Transform *poses = anim->keyframePoses[0];
    const Transform *bind = sayori->skeleton.bindPose;

    Vector3 wrist_posed_pos = poses[wrist_b].translation;
    Quaternion wrist_posed_rot = poses[wrist_b].rotation;

    Quaternion q_w_delta = QuaternionMultiply(wrist_posed_rot, QuaternionInvert(bind[wrist_b].rotation));

    Vector3 off0 = Vector3Subtract(bind[chain[0]].translation, bind[wrist_b].translation);
    poses[chain[0]].translation = Vector3Add(wrist_posed_pos, Vector3RotateByQuaternion(off0, q_w_delta));
    poses[chain[0]].rotation = QuaternionMultiply(QuaternionMultiply(q_w_delta, bind[chain[0]].rotation), q_curl0);

    Quaternion q_p0_delta = QuaternionMultiply(poses[chain[0]].rotation, QuaternionInvert(bind[chain[0]].rotation));
    Vector3 off1 = Vector3Subtract(bind[chain[1]].translation, bind[chain[0]].translation);
    poses[chain[1]].translation = Vector3Add(poses[chain[0]].translation, Vector3RotateByQuaternion(off1, q_p0_delta));
    poses[chain[1]].rotation = QuaternionMultiply(QuaternionMultiply(q_p0_delta, bind[chain[1]].rotation), q_curl1);

    Quaternion q_p1_delta = QuaternionMultiply(poses[chain[1]].rotation, QuaternionInvert(bind[chain[1]].rotation));
    Vector3 off2 = Vector3Subtract(bind[chain[2]].translation, bind[chain[1]].translation);
    poses[chain[2]].translation = Vector3Add(poses[chain[1]].translation, Vector3RotateByQuaternion(off2, q_p1_delta));
    poses[chain[2]].rotation = QuaternionMultiply(QuaternionMultiply(q_p1_delta, bind[chain[2]].rotation), q_curl2);
}

static void ApplySayoriHandGrip(ModelAnimation *anim, Model sayoriModel,
                                int wrist_r, int wrist_l,
                                const SayoriHandFingers *f_r, const SayoriHandFingers *f_l,
                                int cur_wp, float trigger_travel, float wall_lift_t, bool is_sprinting,
                                float jog_blend) {
    if (cur_wp != 1) return;

    Quaternion q_mid0 = QuaternionFromAxisAngle((Vector3){0, 0, 1}, 52.0f * DEG2RAD);
    Quaternion q_mid1 = QuaternionFromAxisAngle((Vector3){0, 0, 1}, 20.0f * DEG2RAD);
    Quaternion q_mid2 = QuaternionFromAxisAngle((Vector3){0, 0, 1}, 10.0f * DEG2RAD);
    ApplyFingerChain(anim, &sayoriModel, wrist_r, f_r->middle, q_mid0, q_mid1, q_mid2);

    Quaternion q_ring0 = QuaternionFromAxisAngle((Vector3){0, 0, 1}, 55.0f * DEG2RAD);
    Quaternion q_ring1 = QuaternionFromAxisAngle((Vector3){0, 0, 1}, 22.0f * DEG2RAD);
    Quaternion q_ring2 = QuaternionFromAxisAngle((Vector3){0, 0, 1}, 10.0f * DEG2RAD);
    ApplyFingerChain(anim, &sayoriModel, wrist_r, f_r->ring, q_ring0, q_ring1, q_ring2);

    Quaternion q_lit0 = QuaternionFromAxisAngle((Vector3){0, 0, 1}, 58.0f * DEG2RAD);
    Quaternion q_lit1 = QuaternionFromAxisAngle((Vector3){0, 0, 1}, 22.0f * DEG2RAD);
    Quaternion q_lit2 = QuaternionFromAxisAngle((Vector3){0, 0, 1}, 10.0f * DEG2RAD);
    ApplyFingerChain(anim, &sayoriModel, wrist_r, f_r->little, q_lit0, q_lit1, q_lit2);

    Quaternion q_th0_r = QuaternionMultiply(QuaternionFromAxisAngle((Vector3){0, 1, 0}, -15.0f * DEG2RAD),
                                            QuaternionFromAxisAngle((Vector3){0, 0, 1}, 12.0f * DEG2RAD));
    Quaternion q_th1_r = QuaternionFromAxisAngle((Vector3){0, 0, 1}, 8.0f * DEG2RAD);
    Quaternion q_th2_r = QuaternionFromAxisAngle((Vector3){0, 0, 1}, 4.0f * DEG2RAD);
    ApplyFingerChain(anim, &sayoriModel, wrist_r, f_r->thumb, q_th0_r, q_th1_r, q_th2_r);

    if (is_sprinting || wall_lift_t > 0.40f) {
        Quaternion q_idx0 = QuaternionFromAxisAngle((Vector3){0, 0, 1}, 6.0f * DEG2RAD);
        Quaternion q_idx1 = QuaternionFromAxisAngle((Vector3){0, 0, 1}, 4.0f * DEG2RAD);
        Quaternion q_idx2 = QuaternionFromAxisAngle((Vector3){0, 0, 1}, 2.0f * DEG2RAD);
        ApplyFingerChain(anim, &sayoriModel, wrist_r, f_r->index, q_idx0, q_idx1, q_idx2);
    } else {
        float base_prep = (cur_wp == 1 && wall_lift_t < 0.2f) ? 36.0f : 32.0f;
        float pull_deg  = base_prep + trigger_travel * 18.0f;
        float mid_curl  = 16.0f + trigger_travel * 9.0f;
        float tip_curl  = 8.0f + trigger_travel * 7.0f;
        Quaternion q_idx0 = QuaternionFromAxisAngle((Vector3){0, 0, 1}, pull_deg * DEG2RAD);
        Quaternion q_idx1 = QuaternionFromAxisAngle((Vector3){0, 0, 1}, mid_curl * DEG2RAD);
        Quaternion q_idx2 = QuaternionFromAxisAngle((Vector3){0, 0, 1}, tip_curl * DEG2RAD);
        ApplyFingerChain(anim, &sayoriModel, wrist_r, f_r->index, q_idx0, q_idx1, q_idx2);
    }

    Quaternion q_idx_wrap0 = QuaternionFromAxisAngle((Vector3){0, 0, 1}, -48.0f * DEG2RAD);
    Quaternion q_idx_wrap1 = QuaternionFromAxisAngle((Vector3){0, 0, 1}, -18.0f * DEG2RAD);
    Quaternion q_idx_wrap2 = QuaternionFromAxisAngle((Vector3){0, 0, 1}, -10.0f * DEG2RAD);

    Quaternion q_mid_wrap0 = QuaternionFromAxisAngle((Vector3){0, 0, 1}, -52.0f * DEG2RAD);
    Quaternion q_mid_wrap1 = QuaternionFromAxisAngle((Vector3){0, 0, 1}, -20.0f * DEG2RAD);
    Quaternion q_mid_wrap2 = QuaternionFromAxisAngle((Vector3){0, 0, 1}, -10.0f * DEG2RAD);

    Quaternion q_ring_wrap0 = QuaternionFromAxisAngle((Vector3){0, 0, 1}, -55.0f * DEG2RAD);
    Quaternion q_ring_wrap1 = QuaternionFromAxisAngle((Vector3){0, 0, 1}, -20.0f * DEG2RAD);
    Quaternion q_ring_wrap2 = QuaternionFromAxisAngle((Vector3){0, 0, 1}, -10.0f * DEG2RAD);

    Quaternion q_lit_wrap0 = QuaternionFromAxisAngle((Vector3){0, 0, 1}, -56.0f * DEG2RAD);
    Quaternion q_lit_wrap1 = QuaternionFromAxisAngle((Vector3){0, 0, 1}, -20.0f * DEG2RAD);
    Quaternion q_lit_wrap2 = QuaternionFromAxisAngle((Vector3){0, 0, 1}, -10.0f * DEG2RAD);

    Quaternion q_th_wrap0 = QuaternionMultiply(QuaternionFromAxisAngle((Vector3){0, 1, 0}, 16.0f * DEG2RAD),
                                               QuaternionFromAxisAngle((Vector3){0, 0, 1}, -12.0f * DEG2RAD));
    Quaternion q_th_wrap1 = QuaternionFromAxisAngle((Vector3){0, 0, 1}, -8.0f * DEG2RAD);
    Quaternion q_th_wrap2 = QuaternionFromAxisAngle((Vector3){0, 0, 1}, -4.0f * DEG2RAD);

    if (jog_blend > 0.05f) {
        Quaternion q_idx_jog0 = QuaternionFromAxisAngle((Vector3){0, 0, 1}, 60.0f * DEG2RAD);
        Quaternion q_idx_jog1 = QuaternionFromAxisAngle((Vector3){0, 0, 1}, 50.0f * DEG2RAD);
        Quaternion q_idx_jog2 = QuaternionFromAxisAngle((Vector3){0, 0, 1}, 25.0f * DEG2RAD);

        Quaternion q_mid_jog0 = QuaternionFromAxisAngle((Vector3){0, 0, 1}, 65.0f * DEG2RAD);
        Quaternion q_mid_jog1 = QuaternionFromAxisAngle((Vector3){0, 0, 1}, 55.0f * DEG2RAD);
        Quaternion q_mid_jog2 = QuaternionFromAxisAngle((Vector3){0, 0, 1}, 28.0f * DEG2RAD);

        Quaternion q_ring_jog0 = QuaternionFromAxisAngle((Vector3){0, 0, 1}, 68.0f * DEG2RAD);
        Quaternion q_ring_jog1 = QuaternionFromAxisAngle((Vector3){0, 0, 1}, 55.0f * DEG2RAD);
        Quaternion q_ring_jog2 = QuaternionFromAxisAngle((Vector3){0, 0, 1}, 30.0f * DEG2RAD);

        Quaternion q_lit_jog0 = QuaternionFromAxisAngle((Vector3){0, 0, 1}, 70.0f * DEG2RAD);
        Quaternion q_lit_jog1 = QuaternionFromAxisAngle((Vector3){0, 0, 1}, 58.0f * DEG2RAD);
        Quaternion q_lit_jog2 = QuaternionFromAxisAngle((Vector3){0, 0, 1}, 30.0f * DEG2RAD);

        Quaternion q_th_jog0 = QuaternionMultiply(QuaternionFromAxisAngle((Vector3){0, 1, 0}, -20.0f * DEG2RAD),
                                                  QuaternionFromAxisAngle((Vector3){0, 0, 1}, 28.0f * DEG2RAD));
        Quaternion q_th_jog1 = QuaternionFromAxisAngle((Vector3){0, 0, 1}, 20.0f * DEG2RAD);
        Quaternion q_th_jog2 = QuaternionFromAxisAngle((Vector3){0, 0, 1}, 12.0f * DEG2RAD);

        ApplyFingerChain(anim, &sayoriModel, wrist_l, f_l->index,  QuaternionSlerp(q_idx_wrap0,  q_idx_jog0,  jog_blend),
                                                                  QuaternionSlerp(q_idx_wrap1,  q_idx_jog1,  jog_blend),
                                                                  QuaternionSlerp(q_idx_wrap2,  q_idx_jog2,  jog_blend));
        ApplyFingerChain(anim, &sayoriModel, wrist_l, f_l->middle, QuaternionSlerp(q_mid_wrap0,  q_mid_jog0,  jog_blend),
                                                                  QuaternionSlerp(q_mid_wrap1,  q_mid_jog1,  jog_blend),
                                                                  QuaternionSlerp(q_mid_wrap2,  q_mid_jog2,  jog_blend));
        ApplyFingerChain(anim, &sayoriModel, wrist_l, f_l->ring,   QuaternionSlerp(q_ring_wrap0, q_ring_jog0, jog_blend),
                                                                  QuaternionSlerp(q_ring_wrap1, q_ring_jog1, jog_blend),
                                                                  QuaternionSlerp(q_ring_wrap2, q_ring_jog2, jog_blend));
        ApplyFingerChain(anim, &sayoriModel, wrist_l, f_l->little, QuaternionSlerp(q_lit_wrap0,  q_lit_jog0,  jog_blend),
                                                                  QuaternionSlerp(q_lit_wrap1,  q_lit_jog1,  jog_blend),
                                                                  QuaternionSlerp(q_lit_wrap2,  q_lit_jog2,  jog_blend));
        ApplyFingerChain(anim, &sayoriModel, wrist_l, f_l->thumb,  QuaternionSlerp(q_th_wrap0,   q_th_jog0,   jog_blend),
                                                                  QuaternionSlerp(q_th_wrap1,   q_th_jog1,   jog_blend),
                                                                  QuaternionSlerp(q_th_wrap2,   q_th_jog2,   jog_blend));
    } else {
        ApplyFingerChain(anim, &sayoriModel, wrist_l, f_l->index,  q_idx_wrap0,  q_idx_wrap1,  q_idx_wrap2);
        ApplyFingerChain(anim, &sayoriModel, wrist_l, f_l->middle, q_mid_wrap0,  q_mid_wrap1,  q_mid_wrap2);
        ApplyFingerChain(anim, &sayoriModel, wrist_l, f_l->ring,   q_ring_wrap0, q_ring_wrap1, q_ring_wrap2);
        ApplyFingerChain(anim, &sayoriModel, wrist_l, f_l->little, q_lit_wrap0,  q_lit_wrap1,  q_lit_wrap2);
        ApplyFingerChain(anim, &sayoriModel, wrist_l, f_l->thumb,  q_th_wrap0,   q_th_wrap1,   q_th_wrap2);
    }
}

typedef struct {
    Vector3 pos;
    Vector3 vel;
    float life;
    float max_life;
    Color color;
} BulletSpark;
#define MAX_SPARKS 64
static BulletSpark world_sparks[MAX_SPARKS] = {0};

static void SpawnBulletSparks(Vector3 hit_pos, Vector3 norm, Color col) {
    for (int k = 0; k < 6; k++) {
        for (int i = 0; i < MAX_SPARKS; i++) {
            if (world_sparks[i].life <= 0.0f) {
                world_sparks[i].pos = hit_pos;
                float rx = ((float)GetRandomValue(-100, 100) / 100.0f) * 1.5f;
                float ry = ((float)GetRandomValue(50, 200) / 100.0f) * 1.5f;
                float rz = ((float)GetRandomValue(-100, 100) / 100.0f) * 1.5f;
                world_sparks[i].vel = (Vector3){ norm.x * 2.0f + rx, norm.y * 2.0f + ry, norm.z * 2.0f + rz };
                world_sparks[i].life = (float)GetRandomValue(15, 35) / 100.0f;
                world_sparks[i].max_life = world_sparks[i].life;
                world_sparks[i].color = col;
                break;
            }
        }
    }
}

static pthread_mutex_t input_mutex = PTHREAD_MUTEX_INITIALIZER;
static InputPacket current_input = {0};

void *input_listener(void *arg) {
  InputPacket packet;
  while (1) {
    size_t bytes_read = fread(&packet, 1, sizeof(InputPacket), stdin);
    if (bytes_read == sizeof(InputPacket)) {
      pthread_mutex_lock(&input_mutex);
      current_input.w = packet.w;
      current_input.s = packet.s;
      current_input.a = packet.a;
      current_input.d = packet.d;
      current_input.left = packet.left;
      current_input.right = packet.right;
      current_input.up = packet.up;
      current_input.down = packet.down;
      current_input.space = packet.space;
      current_input.shift = packet.shift;
      current_input.ctrl = packet.ctrl;
      current_input.shoot = packet.shoot;
      current_input.mouse_dx += packet.mouse_dx;
      current_input.mouse_dy += packet.mouse_dy;

      if (is_arena_mode != 1) {
        current_cam.time_of_day = packet.time_of_day;
      }
      current_cam.lighting_quality = packet.lighting_quality;
      current_cam.shadows_enabled = packet.shadows_enabled;
      current_input.active_weapon = packet.active_weapon;
      current_input.bloom_enabled = packet.bloom_enabled;
      current_input.volumetric_clouds = packet.volumetric_clouds;
      current_input.rain_intensity = packet.rain_intensity;
      current_input.soft_shadows = packet.soft_shadows;
      current_input.is_aiming = packet.is_aiming;
      current_input.pistol_lvl = packet.pistol_lvl;
      current_input.shotgun_lvl = packet.shotgun_lvl;
      current_input.minigun_lvl = packet.minigun_lvl;
      current_input.flashlight_active = packet.flashlight_active;
      current_input.wp_hip_x = packet.wp_hip_x;
      current_input.wp_hip_y = packet.wp_hip_y;
      current_input.wp_hip_z = packet.wp_hip_z;
      current_input.wp_ads_x = packet.wp_ads_x;
      current_input.wp_ads_y = packet.wp_ads_y;
      current_input.wp_ads_z = packet.wp_ads_z;
      current_input.wp_pitch = packet.wp_pitch;
      current_input.wp_yaw = packet.wp_yaw;
      current_input.wp_roll = packet.wp_roll;
      current_input.wp_scale = packet.wp_scale;
      current_input.motion_blur_strength = packet.motion_blur_strength;
      pthread_mutex_unlock(&input_mutex);
    } else {
      exit(0);
    }
  }
  return NULL;
}

static int current_round = 0;
static float inter_round_timer = 0.0f;
static int yuritler_count = 0;
static int sniper_count = 0;
static float total_arena_time = 0.0f;
static float weather_timer = 5.0f;
static int weather_active = 0;

void StartNextArenaRound() {
  current_round++;
  inter_round_timer = 0.0f;
  SendEvent(30, current_round);

  // clean up bodys from previous rounds
  int write_idx = 0;
  for (int i = 0; i < num_enemies; i++) {
    if (enemies[i].state < 3) {
      enemies[write_idx++] = enemies[i];
    }
  }
  num_enemies = write_idx;

  // standard enemies (current_round guards)
  for (int s = 0; s < current_round; s++) {
    if (num_enemies >= MAX_ENEMIES || num_spawn_points == 0) break;
    int sp = rand() % num_spawn_points;
    float jx = ((float)(rand() % 100) / 100.0f - 0.5f) * 0.6f;
    float jz = ((float)(rand() % 100) / 100.0f - 0.5f) * 0.6f;
    enemies[num_enemies].x = spawn_points[sp].x + jx;
    enemies[num_enemies].z = spawn_points[sp].z + jz;
    enemies[num_enemies].type = 4;
    enemies[num_enemies].hp = 100.0f;
    enemies[num_enemies].max_hp = 100.0f;
    enemies[num_enemies].move_speed = 1.5f + ((float)(rand() % 100) / 100.0f - 0.5f) * 0.2f;
    enemies[num_enemies].damage = 10.0f;
    enemies[num_enemies].attack_range = 15.0f;
    enemies[num_enemies].timer = 1.0f;
    enemies[num_enemies].state = 1;
    enemies[num_enemies].y = 2.0f;
    enemies[num_enemies].move_timer = 0.0f;
    enemies[num_enemies].anim_frame = 0;
    enemies[num_enemies].burst_count = 0;
    enemies[num_enemies].reload_timer = 0.0f;
    enemies[num_enemies].dodge_timer = 0.0f;
    num_enemies++;
  }

  // yuritler Boss, every 10 rounds or 15% on even rounds
  int spawn_yuritler = 0;
  if (current_round % 10 == 0) {
    spawn_yuritler = 1;
  } else if (current_round % 2 == 0) {
    if (rand() % 100 < 15) spawn_yuritler = 1;
  }
  if (spawn_yuritler && num_enemies < MAX_ENEMIES && num_spawn_points > 0) {
    yuritler_count++;
    int sp = rand() % num_spawn_points;
    float jx = ((float)(rand() % 100) / 100.0f - 0.5f) * 0.6f;
    float jz = ((float)(rand() % 100) / 100.0f - 0.5f) * 0.6f;
    enemies[num_enemies].x = spawn_points[sp].x + jx;
    enemies[num_enemies].z = spawn_points[sp].z + jz;
    enemies[num_enemies].type = 9;
    enemies[num_enemies].hp = 150.0f + (yuritler_count - 1) * 50.0f;
    enemies[num_enemies].max_hp = enemies[num_enemies].hp;
    enemies[num_enemies].move_speed = 1.8f;
    enemies[num_enemies].damage = 5.0f;
    enemies[num_enemies].attack_range = 20.0f;
    enemies[num_enemies].timer = 1.0f;
    enemies[num_enemies].state = 1;
    enemies[num_enemies].y = 2.0f;
    enemies[num_enemies].move_timer = 0.0f;
    enemies[num_enemies].anim_frame = 0;
    enemies[num_enemies].burst_count = 0;
    enemies[num_enemies].reload_timer = 0.0f;
    enemies[num_enemies].dodge_timer = 0.0f;
    num_enemies++;
  }

  // elite enemies, every 5 rounds
  if (current_round % 5 == 0) {
    int num_elites = current_round / 5;
    for (int e = 0; e < num_elites; e++) {
      if (num_enemies >= MAX_ENEMIES || num_spawn_points == 0) break;
      int sp = rand() % num_spawn_points;
      float jx = ((float)(rand() % 100) / 100.0f - 0.5f) * 0.6f;
      float jz = ((float)(rand() % 100) / 100.0f - 0.5f) * 0.6f;
      enemies[num_enemies].x = spawn_points[sp].x + jx;
      enemies[num_enemies].z = spawn_points[sp].z + jz;
      enemies[num_enemies].type = 2;
      enemies[num_enemies].hp = 100.0f;
      enemies[num_enemies].max_hp = 100.0f;
      enemies[num_enemies].move_speed = 1.5f + ((float)(rand() % 100) / 100.0f - 0.5f) * 0.2f;
      enemies[num_enemies].damage = 4.0f;
      enemies[num_enemies].attack_range = 16.0f;
      enemies[num_enemies].timer = 1.0f;
      enemies[num_enemies].state = 1;
      enemies[num_enemies].y = 2.0f;
      enemies[num_enemies].move_timer = 0.0f;
      enemies[num_enemies].anim_frame = 0;
      enemies[num_enemies].burst_count = 0;
      enemies[num_enemies].reload_timer = 0.0f;
      enemies[num_enemies].dodge_timer = 0.0f;
      num_enemies++;
    }
  }

  // snipers, ddd rounds: 50% chance
  if (current_round % 2 != 0) {
    if (rand() % 100 < 50) {
      sniper_count++;
      for (int sn = 0; sn < sniper_count; sn++) {
        if (num_enemies >= MAX_ENEMIES || num_spawn_points == 0) break;
        int sp = rand() % num_spawn_points;
        float jx = ((float)(rand() % 100) / 100.0f - 0.5f) * 0.6f;
        float jz = ((float)(rand() % 100) / 100.0f - 0.5f) * 0.6f;
        enemies[num_enemies].x = spawn_points[sp].x + jx;
        enemies[num_enemies].z = spawn_points[sp].z + jz;
        enemies[num_enemies].type = 3;
        enemies[num_enemies].hp = 100.0f;
        enemies[num_enemies].max_hp = 100.0f;
        enemies[num_enemies].move_speed = 3.0f;
        enemies[num_enemies].damage = 20.0f;
        enemies[num_enemies].attack_range = 28.0f;
        enemies[num_enemies].timer = 1.8f;
        enemies[num_enemies].state = 1;
        enemies[num_enemies].y = 2.0f;
        enemies[num_enemies].move_timer = 0.0f;
        enemies[num_enemies].anim_frame = 0;
        enemies[num_enemies].burst_count = 0;
        enemies[num_enemies].reload_timer = 0.0f;
        enemies[num_enemies].dodge_timer = 0.0f;
        num_enemies++;
      }
    }
  }
}

typedef struct {
  Vector4 planes[6]; // left, right, bottom, top, near, far
} Frustum;

static inline Frustum ExtractFrustum(Matrix viewProj) {
  Frustum f;
  f.planes[0] = (Vector4){viewProj.m3 + viewProj.m0, viewProj.m7 + viewProj.m4, viewProj.m11 + viewProj.m8, viewProj.m15 + viewProj.m12};
  f.planes[1] = (Vector4){viewProj.m3 - viewProj.m0, viewProj.m7 - viewProj.m4, viewProj.m11 - viewProj.m8, viewProj.m15 - viewProj.m12};
  f.planes[2] = (Vector4){viewProj.m3 + viewProj.m1, viewProj.m7 + viewProj.m5, viewProj.m11 + viewProj.m9, viewProj.m15 + viewProj.m13};
  f.planes[3] = (Vector4){viewProj.m3 - viewProj.m1, viewProj.m7 - viewProj.m5, viewProj.m11 - viewProj.m9, viewProj.m15 - viewProj.m13};
  f.planes[4] = (Vector4){viewProj.m3 + viewProj.m2, viewProj.m7 + viewProj.m6, viewProj.m11 + viewProj.m10, viewProj.m15 + viewProj.m14};
  f.planes[5] = (Vector4){viewProj.m3 - viewProj.m2, viewProj.m7 - viewProj.m6, viewProj.m11 - viewProj.m10, viewProj.m15 - viewProj.m14};
  for (int i = 0; i < 6; i++) {
    float len = sqrtf(f.planes[i].x * f.planes[i].x + f.planes[i].y * f.planes[i].y + f.planes[i].z * f.planes[i].z);
    if (len > 1e-6f) {
      f.planes[i].x /= len;
      f.planes[i].y /= len;
      f.planes[i].z /= len;
      f.planes[i].w /= len;
    }
  }
  return f;
}

static inline bool FrustumContainsBox(Frustum f, BoundingBox box) {
  for (int i = 0; i < 6; i++) {
    Vector4 p = f.planes[i];
    float px = (p.x > 0.0f) ? box.max.x : box.min.x;
    float py = (p.y > 0.0f) ? box.max.y : box.min.y;
    float pz = (p.z > 0.0f) ? box.max.z : box.min.z;
    if (p.x * px + p.y * py + p.z * pz + p.w < 0.0f) {
      return false;
    }
  }
  return true;
}

typedef struct {
  Vector3 v0, v1, v2;
  Vector3 normal;
  float minY, maxY;
  int is_walkable;
} WorldTri;

typedef struct {
  int *indices;
  int count;
  int capacity;
} WorldGridCell;

typedef struct {
  float minX, minZ;
  float maxX, maxZ;
  float cellSize;
  int cols, rows;
  WorldGridCell *cells;
  WorldTri *triangles;
  int tri_count;
  bool is_initialized;
} WorldGrid;

static WorldGrid world_grid = {0};

static inline float DistPointSegment2D(float px, float pz, float ax, float az,
                                       float bx, float bz, float *closestX,
                                       float *closestZ) {
  float abx = bx - ax;
  float abz = bz - az;
  float apx = px - ax;
  float apz = pz - az;

  float ab_len_sq = abx * abx + abz * abz;
  if (ab_len_sq < 1e-8f) {
    *closestX = ax;
    *closestZ = az;
    return sqrtf(apx * apx + apz * apz);
  }

  float t = (apx * abx + apz * abz) / ab_len_sq;
  if (t < 0.0f)
    t = 0.0f;
  else if (t > 1.0f)
    t = 1.0f;

  *closestX = ax + t * abx;
  *closestZ = az + t * abz;
  float dx = px - *closestX;
  float dz = pz - *closestZ;
  return sqrtf(dx * dx + dz * dz);
}

static inline bool PointInTriangle2D(float px, float pz, Vector3 a, Vector3 b,
                                     Vector3 c, float *outY) {
  float v0x = c.x - a.x, v0z = c.z - a.z;
  float v1x = b.x - a.x, v1z = b.z - a.z;
  float v2x = px - a.x,  v2z = pz - a.z;

  float dot00 = v0x * v0x + v0z * v0z;
  float dot01 = v0x * v1x + v0z * v1z;
  float dot02 = v0x * v2x + v0z * v2z;
  float dot11 = v1x * v1x + v1z * v1z;
  float dot12 = v1x * v2x + v1z * v2z;

  float invDenom = 1.0f / (dot00 * dot11 - dot01 * dot01 + 1e-10f);
  float u = (dot11 * dot02 - dot01 * dot12) * invDenom;
  float v = (dot00 * dot12 - dot01 * dot02) * invDenom;

  const float eps = -0.015f;
  if (u >= eps && v >= eps && (u + v) <= (1.0f - eps)) {
    *outY = a.y + v * (b.y - a.y) + u * (c.y - a.y);
    return true;
  }
  return false;
}

void InitWorldGrid(Model model, float cellSize) {
  if (world_grid.is_initialized) return;

  float minX = 1e9f, minZ = 1e9f, maxX = -1e9f, maxZ = -1e9f;
  int total_tris = 0;
  for (int m = 0; m < model.meshCount; m++) {
    Mesh *mesh = &model.meshes[m];
    total_tris += mesh->triangleCount;
    for (int v = 0; v < mesh->vertexCount; v++) {
      float x = mesh->vertices[v * 3];
      float z = mesh->vertices[v * 3 + 2];
      if (x < minX) minX = x;
      if (x > maxX) maxX = x;
      if (z < minZ) minZ = z;
      if (z > maxZ) maxZ = z;
    }
  }

  minX -= 4.0f; minZ -= 4.0f;
  maxX += 4.0f; maxZ += 4.0f;

  world_grid.minX = minX;
  world_grid.minZ = minZ;
  world_grid.maxX = maxX;
  world_grid.maxZ = maxZ;
  world_grid.cellSize = cellSize;
  world_grid.cols = (int)ceilf((maxX - minX) / cellSize);
  world_grid.rows = (int)ceilf((maxZ - minZ) / cellSize);
  world_grid.tri_count = total_tris;
  world_grid.triangles = (WorldTri *)malloc(total_tris * sizeof(WorldTri));
  world_grid.cells = (WorldGridCell *)calloc(world_grid.cols * world_grid.rows, sizeof(WorldGridCell));

  int tri_idx = 0;
  for (int m = 0; m < model.meshCount; m++) {
    Mesh *mesh = &model.meshes[m];
    for (int t = 0; t < mesh->triangleCount; t++) {
      int i0 = mesh->indices ? mesh->indices[t * 3] : (t * 3);
      int i1 = mesh->indices ? mesh->indices[t * 3 + 1] : (t * 3 + 1);
      int i2 = mesh->indices ? mesh->indices[t * 3 + 2] : (t * 3 + 2);

      Vector3 v0 = {mesh->vertices[i0 * 3], mesh->vertices[i0 * 3 + 1], mesh->vertices[i0 * 3 + 2]};
      Vector3 v1 = {mesh->vertices[i1 * 3], mesh->vertices[i1 * 3 + 1], mesh->vertices[i1 * 3 + 2]};
      Vector3 v2 = {mesh->vertices[i2 * 3], mesh->vertices[i2 * 3 + 1], mesh->vertices[i2 * 3 + 2]};

      Vector3 e1 = {v1.x - v0.x, v1.y - v0.y, v1.z - v0.z};
      Vector3 e2 = {v2.x - v0.x, v2.y - v0.y, v2.z - v0.z};
      Vector3 n = {
        e1.y * e2.z - e1.z * e2.y,
        e1.z * e2.x - e1.x * e2.z,
        e1.x * e2.y - e1.y * e2.x
      };
      float nlen = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
      if (nlen > 1e-6f) {
        n.x /= nlen; n.y /= nlen; n.z /= nlen;
      }

      WorldTri *wt = &world_grid.triangles[tri_idx];
      wt->v0 = v0; wt->v1 = v1; wt->v2 = v2;
      wt->normal = n;
      wt->minY = fminf(v0.y, fminf(v1.y, v2.y));
      wt->maxY = fmaxf(v0.y, fmaxf(v1.y, v2.y));
      wt->is_walkable = (n.y >= 0.70f);

      float t_minX = fminf(v0.x, fminf(v1.x, v2.x));
      float t_maxX = fmaxf(v0.x, fmaxf(v1.x, v2.x));
      float t_minZ = fminf(v0.z, fminf(v1.z, v2.z));
      float t_maxZ = fmaxf(v0.z, fmaxf(v1.z, v2.z));

      int c0 = (int)floorf((t_minX - minX) / cellSize);
      int c1 = (int)floorf((t_maxX - minX) / cellSize);
      int r0 = (int)floorf((t_minZ - minZ) / cellSize);
      int r1 = (int)floorf((t_maxZ - minZ) / cellSize);

      if (c0 < 0) c0 = 0; if (c1 >= world_grid.cols) c1 = world_grid.cols - 1;
      if (r0 < 0) r0 = 0; if (r1 >= world_grid.rows) r1 = world_grid.rows - 1;

      for (int r = r0; r <= r1; r++) {
        for (int c = c0; c <= c1; c++) {
          WorldGridCell *cell = &world_grid.cells[r * world_grid.cols + c];
          if (cell->count >= cell->capacity) {
            cell->capacity = cell->capacity == 0 ? 8 : cell->capacity * 2;
            cell->indices = (int *)realloc(cell->indices, cell->capacity * sizeof(int));
          }
          cell->indices[cell->count++] = tri_idx;
        }
      }
      tri_idx++;
    }
  }
  world_grid.is_initialized = true;
}

void FreeWorldGrid(void) {
  if (!world_grid.is_initialized) return;
  if (world_grid.cells != NULL) {
    int total_cells = world_grid.cols * world_grid.rows;
    for (int i = 0; i < total_cells; i++) {
      if (world_grid.cells[i].indices != NULL) {
        free(world_grid.cells[i].indices);
      }
    }
    free(world_grid.cells);
    world_grid.cells = NULL;
  }
  if (world_grid.triangles != NULL) {
    free(world_grid.triangles);
    world_grid.triangles = NULL;
  }
  world_grid.is_initialized = false;
}

float GetGroundHeight(float px, float pz, float current_y, float max_step) {
  if (!world_grid.is_initialized) return 0.0f;

  int col = (int)floorf((px - world_grid.minX) / world_grid.cellSize);
  int row = (int)floorf((pz - world_grid.minZ) / world_grid.cellSize);

  float best_y = -999.0f;
  float max_allowed_y = current_y + max_step;

  if (col >= 0 && col < world_grid.cols && row >= 0 && row < world_grid.rows) {
    WorldGridCell *cell = &world_grid.cells[row * world_grid.cols + col];
    for (int i = 0; i < cell->count; i++) {
      WorldTri *wt = &world_grid.triangles[cell->indices[i]];
      if (!wt->is_walkable) continue;

      float tri_y = 0.0f;
      if (PointInTriangle2D(px, pz, wt->v0, wt->v1, wt->v2, &tri_y)) {
        if (tri_y <= max_allowed_y && tri_y > best_y) {
          best_y = tri_y;
        }
      }
    }
  }

  if (best_y < -900.0f && col >= 0 && col < world_grid.cols && row >= 0 && row < world_grid.rows) {
    const float cross_off[4][2] = {{0.08f, 0.0f}, {-0.08f, 0.0f}, {0.0f, 0.08f}, {0.0f, -0.08f}};
    WorldGridCell *cell = &world_grid.cells[row * world_grid.cols + col];
    for (int s = 0; s < 4; s++) {
      float sx = px + cross_off[s][0];
      float sz = pz + cross_off[s][1];
      for (int i = 0; i < cell->count; i++) {
        WorldTri *wt = &world_grid.triangles[cell->indices[i]];
        if (!wt->is_walkable) continue;
        float tri_y = 0.0f;
        if (PointInTriangle2D(sx, sz, wt->v0, wt->v1, wt->v2, &tri_y)) {
          if (tri_y <= max_allowed_y && tri_y > best_y) {
            best_y = tri_y;
          }
        }
      }
      if (best_y > -900.0f) break;
    }
  }

  if (best_y < -900.0f) {
    if (current_y > -500.0f && current_y > -0.5f) {
      best_y = current_y;
    } else if (px >= -40.0f && px <= 40.0f && pz >= -31.0f && pz <= 31.0f) {
      best_y = -1.09f;
    } else {
      best_y = -1000.0f;
    }
  }
  return best_y;
}

void ResolveWalls(float *px, float *pz, float player_y, float radius, float height, float step_height) {
  if (!world_grid.is_initialized) return;

  float p_minY = player_y + step_height;
  float p_maxY = player_y + height;

  int c0 = (int)floorf((*px - radius - world_grid.minX) / world_grid.cellSize);
  int c1 = (int)floorf((*px + radius - world_grid.minX) / world_grid.cellSize);
  int r0 = (int)floorf((*pz - radius - world_grid.minZ) / world_grid.cellSize);
  int r1 = (int)floorf((*pz + radius - world_grid.minZ) / world_grid.cellSize);

  if (c0 < 0) c0 = 0; if (c1 >= world_grid.cols) c1 = world_grid.cols - 1;
  if (r0 < 0) r0 = 0; if (r1 >= world_grid.rows) r1 = world_grid.rows - 1;

  for (int iter = 0; iter < 3; iter++) {
    for (int r = r0; r <= r1; r++) {
      for (int c = c0; c <= c1; c++) {
        WorldGridCell *cell = &world_grid.cells[r * world_grid.cols + c];
        for (int i = 0; i < cell->count; i++) {
          WorldTri *wt = &world_grid.triangles[cell->indices[i]];
          if (wt->is_walkable) continue;

          if (wt->maxY <= p_minY || wt->minY >= p_maxY) continue;

          Vector3 pts[3] = {wt->v0, wt->v1, wt->v2};
          for (int e = 0; e < 3; e++) {
            Vector3 a = pts[e];
            Vector3 b = pts[(e + 1) % 3];

            float closeX, closeZ;
            float dist = DistPointSegment2D(*px, *pz, a.x, a.z, b.x, b.z, &closeX, &closeZ);
            if (dist < radius && dist > 1e-5f) {
              float push = radius - dist;
              float nx = (*px - closeX) / dist;
              float nz = (*pz - closeZ) / dist;
              *px += nx * push;
              *pz += nz * push;
            }
          }
        }
      }
    }
  }
}

static float GetForwardWallDistance(Vector3 origin, Vector3 dir, float max_dist, float player_y) {
  float best_dist = max_dist;

  if (is_arena_mode >= 2 && world_grid.is_initialized) {
    Vector3 end_pt = Vector3Add(origin, Vector3Scale(dir, max_dist));
    float minX = fminf(origin.x, end_pt.x) - 0.05f;
    float maxX = fmaxf(origin.x, end_pt.x) + 0.05f;
    float minZ = fminf(origin.z, end_pt.z) - 0.05f;
    float maxZ = fmaxf(origin.z, end_pt.z) + 0.05f;
    float minY = fminf(origin.y, end_pt.y) - 0.06f;
    float maxY = fmaxf(origin.y, end_pt.y) + 0.06f;

    int c0 = (int)floorf((minX - world_grid.minX) / world_grid.cellSize);
    int c1 = (int)floorf((maxX - world_grid.minX) / world_grid.cellSize);
    int r0 = (int)floorf((minZ - world_grid.minZ) / world_grid.cellSize);
    int r1 = (int)floorf((maxZ - world_grid.minZ) / world_grid.cellSize);

    if (c0 < 0) c0 = 0;
    if (c1 >= world_grid.cols) c1 = world_grid.cols - 1;
    if (r0 < 0) r0 = 0;
    if (r1 >= world_grid.rows) r1 = world_grid.rows - 1;

    for (int r = r0; r <= r1; r++) {
      for (int c = c0; c <= c1; c++) {
        WorldGridCell *cell = &world_grid.cells[r * world_grid.cols + c];
        for (int i = 0; i < cell->count; i++) {
          WorldTri *wt = &world_grid.triangles[cell->indices[i]];
          if (wt->is_walkable || fabsf(wt->normal.y) > 0.65f) continue;
          if (wt->maxY < minY || wt->minY > maxY) continue;
          if (wt->maxY <= player_y + 0.45f) continue;

          Vector3 e1 = Vector3Subtract(wt->v1, wt->v0);
          Vector3 e2 = Vector3Subtract(wt->v2, wt->v0);
          Vector3 h  = Vector3CrossProduct(dir, e2);
          float a    = Vector3DotProduct(e1, h);
          if (fabsf(a) < 1e-6f) continue;

          float f = 1.0f / a;
          Vector3 s = Vector3Subtract(origin, wt->v0);
          float u = f * Vector3DotProduct(s, h);
          if (u < -0.005f || u > 1.005f) continue;

          Vector3 q = Vector3CrossProduct(s, e1);
          float v = f * Vector3DotProduct(dir, q);
          if (v < -0.005f || (u + v) > 1.005f) continue;

          float t = f * Vector3DotProduct(e2, q);
          if (t > 0.01f && t < best_dist) {
            best_dist = t;
          }
        }
      }
    }
  } else if (is_arena_mode < 2 && dynamic_map != NULL) {
    int layer = (int)floorf(player_y / 2.0f);
    for (float t = 0.05f; t < max_dist; t += 0.03f) {
      Vector3 p = Vector3Add(origin, Vector3Scale(dir, t));
      int bx = (int)roundf(p.x / 2.0f);
      int bz = (int)roundf(p.z / 2.0f);
      if (GetBlock(bx, layer, bz) > 0) {
        if (t < best_dist) best_dist = t;
        break;
      }
    }
  }

  return best_dist;
}

static void SolveAnatomicalLegIK(float dy, float dz, float L1, float L2, float *out_hip, float *out_knee, float *out_ankle) {
  float dist = sqrtf(dy * dy + dz * dz);
  float max_dist = L1 + L2 - 0.003f;
  float min_dist = fabsf(L1 - L2) + 0.05f;
  if (dist > max_dist) {
    float s = max_dist / dist;
    dy *= s; dz *= s; dist = max_dist;
  } else if (dist < min_dist) {
    float s = min_dist / dist;
    dy *= s; dz *= s; dist = min_dist;
  }

  float psi = atan2f(dz, -dy);
  float cos_alpha = (L1 * L1 + dist * dist - L2 * L2) / (2.0f * L1 * dist);
  cos_alpha = fminf(1.0f, fmaxf(-1.0f, cos_alpha));
  float alpha = acosf(cos_alpha);

  float cos_beta = (L1 * L1 + L2 * L2 - dist * dist) / (2.0f * L1 * L2);
  cos_beta = fminf(1.0f, fmaxf(-1.0f, cos_beta));
  float knee_bend = PI - acosf(cos_beta);

  float thigh_angle = psi + alpha;

  *out_hip = -thigh_angle;
  *out_knee = knee_bend;
  *out_ankle = -(*out_hip + *out_knee);
}

static void GetBiomechanicalFootTrajectory(float phi_in, float stride_reach, float stride_push, float lift_max,
                                           float strafe_reach, float strafe_push,
                                           float *out_dz, float *out_dx, float *out_lift, float *out_ankle_tilt) {
  float phi = fmodf(phi_in, 2.0f * PI);
  if (phi < 0.0f) phi += 2.0f * PI;

  const float phi_toe_off = 1.15f * PI;

  if (phi < phi_toe_off) {
    float u = phi / phi_toe_off;
    *out_dz = stride_reach * (1.0f - u) - stride_push * u;
    *out_dx = strafe_reach * (1.0f - u) - strafe_push * u;
    *out_lift = 0.0f;

    if (u < 0.15f) {
      float t = u / 0.15f;
      *out_ankle_tilt = 12.0f * (1.0f - t);
    } else if (u < 0.70f) {
      *out_ankle_tilt = 0.0f;
    } else {
      float t = (u - 0.70f) / 0.30f;
      *out_ankle_tilt = -25.0f * (t * t);
    }
  } else {
    float s = (phi - phi_toe_off) / (2.0f * PI - phi_toe_off);
    float s_curve = s * s * (3.0f - 2.0f * s);
    *out_dz = -stride_push * (1.0f - s_curve) + stride_reach * s_curve;
    *out_dx = -strafe_push * (1.0f - s_curve) + strafe_reach * s_curve;

    float lift_curve = (s < 0.5f) ? powf(sinf(s * PI), 0.85f) : powf(sinf(s * PI), 1.25f);
    *out_lift = lift_max * lift_curve;

    if (s < 0.30f) {
      float t = s / 0.30f;
      *out_ankle_tilt = -25.0f * (1.0f - t);
    } else if (s < 0.70f) {
      *out_ankle_tilt = 0.0f;
    } else {
      float t = (s - 0.70f) / 0.30f;
      *out_ankle_tilt = 12.0f * t;
    }
  }
}

static Vector3 ClosestPtSegment(Vector3 p, Vector3 a, Vector3 b) {
  Vector3 ab = Vector3Subtract(b, a);
  float d2 = Vector3DotProduct(ab, ab);
  if (d2 < 1e-6f) return a;
  float t = Vector3DotProduct(Vector3Subtract(p, a), ab) / d2;
  t = fmaxf(0.0f, fminf(1.0f, t));
  return Vector3Add(a, Vector3Scale(ab, t));
}

static void ResolveThighClothCollision(Vector3 *p, Vector3 hip, Vector3 knee, float radius, int col, bool is_left_thigh) {
  Vector3 knee_ext = Vector3Add(knee, (Vector3){ 0.0f, -0.015f, 0.025f });
  Vector3 close_pt = ClosestPtSegment(*p, hip, knee_ext);
  float dx = p->x - close_pt.x;
  float dz = p->z - close_pt.z;
  float horiz_dist = sqrtf(dx * dx + dz * dz);

  if (horiz_dist < radius) {
    bool is_front_col = (col <= 2) || (col >= 9);
    bool is_side_col = (col == 3 && is_left_thigh) || (col == 8 && !is_left_thigh);

    if (is_front_col) {
      float target_dz = sqrtf(fmaxf(0.001f, radius * radius - dx * dx));
      if (p->z < close_pt.z + target_dz) {
        p->z = close_pt.z + target_dz;
      }
    } else if (is_side_col) {
      float sign = is_left_thigh ? 1.0f : -1.0f;
      float target_dx = sign * sqrtf(fmaxf(0.001f, radius * radius - dz * dz));
      if (is_left_thigh ? (p->x < close_pt.x + target_dx) : (p->x > close_pt.x + target_dx)) {
        p->x = close_pt.x + target_dx;
      }
    } else {
      float target_dz = -sqrtf(fmaxf(0.001f, radius * radius - dx * dx));
      if (p->z > close_pt.z + target_dz) {
        p->z = close_pt.z + target_dz;
      }
    }
  }
}

static void BuildRollBasis(Vector3 fwd, Vector3 flat_right, float roll_rad, Vector3 *out_right, Vector3 *out_up) {
    Vector3 up0 = Vector3Normalize(Vector3CrossProduct(flat_right, fwd));
    float c = cosf(roll_rad), s = sinf(roll_rad);
    *out_right = Vector3Add(Vector3Scale(flat_right, c), Vector3Scale(up0, -s));
    *out_up    = Vector3Add(Vector3Scale(up0, c), Vector3Scale(flat_right, s));
}

typedef struct {
    Vector3 posed_pos[256];
    Quaternion accum_rot[256];
    Vector3 eye_world;
    Vector3 sh_world_r;
    Vector3 sh_world_l;
} SayoriSpineState;

static void ComputeSayoriSpinePose(SayoriSpineState *out_state,
                                   const Model *sayori,
                                   int s_upper_body, int s_upper_body2,
                                   int s_neck, int s_head,
                                   const SayoriArmChainIdx *ch_r, const SayoriArmChainIdx *ch_l,
                                   float current_cam_pitch, float yaw_diff,
                                   float walk_hip_twist,
                                   float torso_lean_pitch, float torso_lean_roll, float ribcage_breath,
                                   float body_yaw_rad, Vector3 body_pos) {
    if (!sayori || s_upper_body < 0 || s_upper_body2 < 0) return;
    int bcount = sayori->skeleton.boneCount;
    if (bcount > 256) bcount = 256;

    float pitch_up = fmaxf(0.0f, current_cam_pitch / 89.0f);
    float pitch_dn = fmaxf(0.0f, -current_cam_pitch / 89.0f);
    float spine_pitch_deg = 0.0f;
    if (pitch_up > 0.0f) {
        spine_pitch_deg = -10.5f * powf(pitch_up, 1.15f);
    } else if (pitch_dn > 0.0f) {
        spine_pitch_deg = +4.5f * powf(pitch_dn, 1.15f);
    }
    float total_pitch = spine_pitch_deg + torso_lean_pitch;
    float lumbar_pitch = total_pitch * 0.35f;
    float thoracic_pitch = total_pitch * 0.65f;

    float lumbar_roll = torso_lean_roll * 0.35f;
    float thoracic_roll = torso_lean_roll * 0.65f;

    float lumbar_yaw = yaw_diff * 0.38f - walk_hip_twist * RAD2DEG * 0.70f;
    float thoracic_yaw = yaw_diff * 0.44f;

    Quaternion q_lumbar = QuaternionMultiply(
        QuaternionMultiply(
            QuaternionFromAxisAngle((Vector3){ 0, 1, 0 }, -lumbar_yaw * DEG2RAD),
            QuaternionFromAxisAngle((Vector3){ 1, 0, 0 }, lumbar_pitch * DEG2RAD)
        ),
        QuaternionFromAxisAngle((Vector3){ 0, 0, 1 }, lumbar_roll * DEG2RAD)
    );
    Quaternion q_thoracic = QuaternionMultiply(
        QuaternionMultiply(
            QuaternionFromAxisAngle((Vector3){ 0, 1, 0 }, -thoracic_yaw * DEG2RAD),
            QuaternionFromAxisAngle((Vector3){ 1, 0, 0 }, thoracic_pitch * DEG2RAD)
        ),
        QuaternionFromAxisAngle((Vector3){ 0, 0, 1 }, thoracic_roll * DEG2RAD)
    );

    for (int b = 0; b < bcount; b++) {
        out_state->accum_rot[b] = QuaternionIdentity();
        out_state->posed_pos[b] = sayori->skeleton.bindPose[b].translation;
    }
    if (s_upper_body2 >= 0 && s_upper_body2 < bcount) {
        out_state->posed_pos[s_upper_body2].y += ribcage_breath;
    }

    out_state->accum_rot[s_upper_body] = q_lumbar;
    int ub_parent = sayori->skeleton.bones[s_upper_body].parent;
    if (ub_parent >= 0) {
        Vector3 off = Vector3Subtract(sayori->skeleton.bindPose[s_upper_body].translation,
                                      sayori->skeleton.bindPose[ub_parent].translation);
        out_state->posed_pos[s_upper_body] = Vector3Add(sayori->skeleton.bindPose[ub_parent].translation,
                                                        Vector3RotateByQuaternion(off, q_lumbar));
    }

    for (int pass = 0; pass < 8; pass++) {
        for (int b = 0; b < bcount; b++) {
            if (b == s_upper_body) continue;
            int p = sayori->skeleton.bones[b].parent;
            if (p < 0) continue;

            bool p_has = (out_state->accum_rot[p].w != 1.0f || out_state->accum_rot[p].x != 0.0f ||
                          out_state->accum_rot[p].y != 0.0f || out_state->accum_rot[p].z != 0.0f);
            bool c_has = (out_state->accum_rot[b].w != 1.0f || out_state->accum_rot[b].x != 0.0f ||
                          out_state->accum_rot[b].y != 0.0f || out_state->accum_rot[b].z != 0.0f);

            if (p_has && !c_has) {
                if (b == s_upper_body2) {
                    out_state->accum_rot[b] = QuaternionMultiply(out_state->accum_rot[p], q_thoracic);
                } else {
                    out_state->accum_rot[b] = out_state->accum_rot[p];
                }
                Vector3 offset = Vector3Subtract(sayori->skeleton.bindPose[b].translation,
                                                 sayori->skeleton.bindPose[p].translation);
                out_state->posed_pos[b] = Vector3Add(out_state->posed_pos[p],
                                                     Vector3RotateByQuaternion(offset, out_state->accum_rot[p]));
            }
        }
    }

    float shrug_t = Clamp((current_cam_pitch - 15.0f) / 65.0f, 0.0f, 1.0f);
    shrug_t = shrug_t * shrug_t * (3.0f - 2.0f * shrug_t);
    float sh_lift = shrug_t * 0.026f;
    if (sh_lift > 0.0f) {
        if (ch_r) {
            int sh_p = (ch_r->sh >= 0 && ch_r->sh < bcount) ? sayori->skeleton.bones[ch_r->sh].parent : -1;
            if (sh_p >= 0 && sh_p < bcount)            out_state->posed_pos[sh_p].y += sh_lift;
            if (ch_r->sh >= 0 && ch_r->sh < bcount)     out_state->posed_pos[ch_r->sh].y += sh_lift;
            if (ch_r->sh_c >= 0 && ch_r->sh_c < bcount) out_state->posed_pos[ch_r->sh_c].y += sh_lift;
            if (ch_r->arm >= 0 && ch_r->arm < bcount)   out_state->posed_pos[ch_r->arm].y += sh_lift;
        }
        if (ch_l) {
            int sh_p = (ch_l->sh >= 0 && ch_l->sh < bcount) ? sayori->skeleton.bones[ch_l->sh].parent : -1;
            if (sh_p >= 0 && sh_p < bcount)            out_state->posed_pos[sh_p].y += sh_lift;
            if (ch_l->sh >= 0 && ch_l->sh < bcount)     out_state->posed_pos[ch_l->sh].y += sh_lift;
            if (ch_l->sh_c >= 0 && ch_l->sh_c < bcount) out_state->posed_pos[ch_l->sh_c].y += sh_lift;
            if (ch_l->arm >= 0 && ch_l->arm < bcount)   out_state->posed_pos[ch_l->arm].y += sh_lift;
        }
    }

    Matrix bodyMat = MatrixMultiply(
        MatrixRotateY(body_yaw_rad),
        MatrixTranslate(body_pos.x, body_pos.y, body_pos.z)
    );

    if (s_head >= 0) {
        Vector3 eye_local = { 0.0f, +0.004f, +0.076f };
        Vector3 eye_model = Vector3Add(out_state->posed_pos[s_head], Vector3RotateByQuaternion(eye_local, out_state->accum_rot[s_head]));
        out_state->eye_world = Vector3Transform(eye_model, bodyMat);
    } else {
        out_state->eye_world = body_pos;
    }

    if (ch_r && ch_r->arm >= 0) {
        out_state->sh_world_r = Vector3Transform(out_state->posed_pos[ch_r->arm], bodyMat);
    } else {
        out_state->sh_world_r = body_pos;
    }
    if (ch_l && ch_l->arm >= 0) {
        out_state->sh_world_l = Vector3Transform(out_state->posed_pos[ch_l->arm], bodyMat);
    } else {
        out_state->sh_world_l = body_pos;
    }
}

static void TetherGunToShoulders(Matrix *gun, Vector3 sh_r, Vector3 sh_l, const Model *sayori,
                                 const SayoriArmChainIdx *chL, const SayoriArmChainIdx *chR, float ads_eff,
                                 float jog_blend) {
    if (chL->arm < 0 || chR->arm < 0 || chL->elbow < 0 || chR->elbow < 0 || chL->wrist < 0 || chR->wrist < 0) return;

    Vector3 sh[2] = { sh_r, sh_l };
    Vector3 sock[2], fdir[2];
    float reach[2];
    const SayoriArmChainIdx *chs[2] = { chR, chL };
    sock[0] = (Vector3){ +0.015f, -0.022f, +0.008f };
    sock[1] = (Vector3){ -0.016f, -0.026f, -0.002f };
    fdir[0] = Vector3Normalize((Vector3){ -0.22f, -0.17f, -0.96f });
    fdir[1] = Vector3Normalize((Vector3){ +0.28f, -0.14f, -0.95f });
    for (int h = 0; h < 2; h++) {
        Vector3 a = sayori->skeleton.bindPose[chs[h]->arm].translation;
        Vector3 e = sayori->skeleton.bindPose[chs[h]->elbow].translation;
        Vector3 w = sayori->skeleton.bindPose[chs[h]->wrist].translation;
        reach[h] = 0.945f * (Vector3Distance(a, e) + Vector3Distance(e, w));
    }

    Vector3 sight_axis = Vector3Normalize(TransformDirByMatrix((Vector3){ 0.0f, 0.0f, -1.0f }, *gun));
    int hand_count = (jog_blend > 0.40f) ? 1 : 2;

    for (int it = 0; it < 3; it++) {
        for (int h = 0; h < hand_count; h++) {
            Vector3 wr = Vector3Subtract(Vector3Transform(sock[h], *gun),
                                         Vector3Scale(TransformDirByMatrix(fdir[h], *gun), 0.057f));
            Vector3 d = Vector3Subtract(wr, sh[h]);
            float len = Vector3Length(d);
            float excess = len - reach[h];
            if (excess > 0.0f && len > 1e-5f) {
                Vector3 push = Vector3Scale(sight_axis, -excess);
                gun->m12 += push.x;
                gun->m13 += push.y;
                gun->m14 += push.z;
            }
        }

        Vector3 chest_mid = Vector3Scale(Vector3Add(sh[0], sh[1]), 0.5f);
        chest_mid.y -= 0.04f;
        Vector3 grip_w = Vector3Transform((Vector3){ 0.0f, -0.024f, 0.003f }, *gun);
        Vector3 d_chest = Vector3Subtract(grip_w, chest_mid);
        float d_horiz = sqrtf(d_chest.x * d_chest.x + d_chest.z * d_chest.z);
        const float min_chest_dist = 0.170f;
        if (d_horiz < min_chest_dist && d_horiz > 1e-5f && d_chest.y > -0.22f && d_chest.y < 0.22f) {
            float push_mag = min_chest_dist - d_horiz;
            Vector3 push_dir = Vector3Normalize((Vector3){ d_chest.x, 0.0f, d_chest.z });
            gun->m12 += push_dir.x * push_mag;
            gun->m14 += push_dir.z * push_mag;
        }
    }
}

int main(int argc, char *argv[]) {
#ifdef _WIN32
  _setmode(_fileno(stdout), _O_BINARY);
  _setmode(_fileno(stdin), _O_BINARY);
#endif

  int render_width = 800;
  int render_height = 600;

  int init_header[6];
  size_t header_read = fread(init_header, sizeof(int), 6, stdin);

  if (header_read == 6) {
    render_width = init_header[0];
    render_height = init_header[1];
    map_width = init_header[2];
    map_height = init_header[3];
    map_layers = init_header[4];
    is_arena_mode = init_header[5];

    if (map_width > 0 && map_height > 0 && map_layers > 0) {
      int total_tiles = map_width * map_height * map_layers;
      dynamic_map = (int *)malloc(total_tiles * sizeof(int));
      if (dynamic_map != NULL) {
        size_t items_read = 0;
        while (items_read < (size_t)total_tiles) {
          size_t r = fread(dynamic_map + items_read, sizeof(int),
                           total_tiles - items_read, stdin);
          if (r <= 0)
            break;
          items_read += r;
        }
      }
    }

    float start_cam[5];
    fread(start_cam, sizeof(float), 5, stdin);
    current_cam.x = start_cam[0];
    current_cam.y = start_cam[1];
    current_cam.z = start_cam[2];
    current_cam.pitch = start_cam[3];
    current_cam.yaw = start_cam[4];

    fread(&num_enemies, sizeof(int), 1, stdin);
    if (num_enemies > MAX_ENEMIES)
      num_enemies = MAX_ENEMIES;
    fread(&num_static_sprites, sizeof(int), 1, stdin);
    if (num_static_sprites > MAX_SPRITES)
      num_static_sprites = MAX_SPRITES;
    fread(&num_spawn_points, sizeof(int), 1, stdin);

    for (int i = 0; i < num_enemies; i++) {
      fread(&enemies[i].x, sizeof(float), 1, stdin);
      fread(&enemies[i].z, sizeof(float), 1, stdin);
      fread(&enemies[i].type, sizeof(int), 1, stdin);
      fread(&enemies[i].state, sizeof(int), 1, stdin);
      enemies[i].state = 1;
      enemies[i].hp = 100.0f;
      enemies[i].max_hp = 100.0f;
      enemies[i].y = 2.0f;
      enemies[i].move_timer = 0.0f;
      enemies[i].anim_frame = 0;
      enemies[i].burst_count = 0;
      enemies[i].reload_timer = 0.0f;
      enemies[i].dodge_timer = 0.0f;
      enemies[i].timer = (float)(rand() % 100) / 100.0f;

      if (enemies[i].type == 1 || enemies[i].type == 9) { // yuritler
        enemies[i].hp = 150.0f;
        enemies[i].max_hp = 150.0f;
        enemies[i].move_speed = 1.8f;
        enemies[i].damage = 5.0f;
        enemies[i].attack_range = 20.0f;
      } else if (enemies[i].type == 2) { // elite enemies
        enemies[i].move_speed = 1.5f;
        enemies[i].damage = 4.0f;
        enemies[i].attack_range = 16.0f;
      } else if (enemies[i].type == 3) { // sniper
        enemies[i].move_speed = 3.0f;
        enemies[i].damage = 20.0f;
        enemies[i].attack_range = 28.0f;
      } else { // standard enemy
        enemies[i].move_speed = 1.5f;
        enemies[i].damage = 10.0f;
        enemies[i].attack_range = 15.0f;
      }
    }

    for (int i = 0; i < num_static_sprites; i++) {
      fread(&static_sprites[i].x, sizeof(float), 1, stdin);
      fread(&static_sprites[i].z, sizeof(float), 1, stdin);
      fread(&static_sprites[i].type, sizeof(int), 1, stdin);
      static_sprites[i].state = 0;
      static_sprites[i].y = 0.0f;
    }

    if (num_spawn_points > 64)
      num_spawn_points = 64;
    for (int i = 0; i < num_spawn_points; i++) {
      fread(&spawn_points[i].x, sizeof(float), 1, stdin);
      fread(&spawn_points[i].z, sizeof(float), 1, stdin);
    }
  }

  SetTraceLogCallback(CustomLog);
  SetTraceLogLevel(LOG_ALL);

  SetConfigFlags(FLAG_WINDOW_HIDDEN);
  InitWindow(render_width, render_height, "Raylib Server");
  SetTargetFPS(60);

  Texture2D wall_textures[9];
  wall_textures[0] = LoadFlippedTexture("game/pics/walls/eagle.png");
  wall_textures[1] = LoadFlippedTexture("game/pics/walls/redbrick.png");
  wall_textures[2] = LoadFlippedTexture("game/pics/walls/purplestone.png");
  wall_textures[3] = LoadFlippedTexture("game/pics/walls/greystone.png");
  wall_textures[4] = LoadFlippedTexture("game/pics/walls/bluestone.png");
  wall_textures[5] = LoadFlippedTexture("game/pics/walls/mossy.png");
  wall_textures[6] = LoadFlippedTexture("game/pics/walls/wood.png");
  wall_textures[7] = LoadFlippedTexture("game/pics/walls/colorstone.png");
  wall_textures[8] = LoadFlippedTexture("game/pics/walls/cement.png");

  Texture2D floor_tex = LoadFlippedTexture("game/pics/walls/colorstone.png");

  Texture2D sprite_tex[16];
  sprite_tex[0] = LoadTexture("game/pics/items/barrel.png");
  sprite_tex[1] = LoadTexture("game/pics/items/pillar.png");
  sprite_tex[2] = LoadTexture("game/pics/items/greenlight.png");
  sprite_tex[3] = LoadTexture("game/pics/items/pillar_destroyed.png");
  sprite_tex[4] = LoadTexture("game/pics/enemies/guard.png");
  sprite_tex[5] = LoadTexture("game/pics/enemies/guard_d.png");
  sprite_tex[6] = LoadTexture("game/pics/items/bullet.png");
  sprite_tex[7] = LoadTexture("game/pics/items/medkit.png");
  sprite_tex[8] = LoadTexture("game/pics/items/cookie.png");
  sprite_tex[9] = LoadTexture("game/pics/enemies/yuritler.png");
  sprite_tex[10] = LoadTexture("game/pics/enemies/yuritler_d.png");
  sprite_tex[11] = LoadTexture("game/pics/items/coins.png");
  sprite_tex[12] = LoadTexture("game/pics/items/coins.png");
  sprite_tex[13] = LoadTexture("game/pics/items/random_gun_i.png");
  sprite_tex[14] = LoadTexture("game/pics/items/bullet_red.png");
  sprite_tex[15] = LoadTexture("game/pics/items/minigun.png");

  Model guyModel = LoadModel("game/models/monika_walk.glb");
  
  Model weaponModels[4];
  const char *weapon_model_paths[4] = {
      "game/models/crimson.glb",
      "game/models/m1911.glb",
      "game/models/shotgun.glb",
      "game/models/minigun.glb"
  };
  for (int i = 0; i < 4; i++) {
      if (FileExists(weapon_model_paths[i])) {
          weaponModels[i] = LoadModel(weapon_model_paths[i]);
      } else {
          weaponModels[i] = LoadModel("game/models/hk_usp.glb");
      }
  }

  ModelAnimation m1911_anim = {0};
  bool has_m1911_anim = false;
  int m1911_b_slide = 1, m1911_b_hammer = 2, m1911_b_trigger = 3;
  int m1911_b_grip_safety = 5, m1911_b_barrel = 6;
  if (weaponModels[1].skeleton.boneCount >= 6) {
      for (int b = 0; b < weaponModels[1].skeleton.boneCount; b++) {
          const char *bn = weaponModels[1].skeleton.bones[b].name;
          if (strcmp(bn, "bone_slide") == 0) m1911_b_slide = b;
          else if (strcmp(bn, "bone_hammer") == 0) m1911_b_hammer = b;
          else if (strcmp(bn, "bone_trigger") == 0) m1911_b_trigger = b;
          else if (strcmp(bn, "bone_grip_safety") == 0) m1911_b_grip_safety = b;
          else if (strcmp(bn, "bone_barrel") == 0) m1911_b_barrel = b;
      }
      m1911_anim.boneCount = weaponModels[1].skeleton.boneCount;
      m1911_anim.keyframeCount = 1;
      m1911_anim.keyframePoses = (ModelAnimPose *)malloc(sizeof(ModelAnimPose));
      if (m1911_anim.keyframePoses != NULL) {
          m1911_anim.keyframePoses[0] = (Transform *)malloc(m1911_anim.boneCount * sizeof(Transform));
          for (int b = 0; b < (int)m1911_anim.boneCount; b++) {
              m1911_anim.keyframePoses[0][b] = weaponModels[1].skeleton.bindPose[b];
          }
          if (m1911_b_hammer < m1911_anim.boneCount) {
              m1911_anim.keyframePoses[0][m1911_b_hammer].rotation =
                  QuaternionFromAxisAngle((Vector3){1, 0, 0}, 48.0f * DEG2RAD);
          }
          if (m1911_b_grip_safety < m1911_anim.boneCount) {
              m1911_anim.keyframePoses[0][m1911_b_grip_safety].rotation =
                  QuaternionFromAxisAngle((Vector3){1, 0, 0}, 5.0f * DEG2RAD);
          }
          UpdateModelAnimation(weaponModels[1], m1911_anim, 0);
          has_m1911_anim = true;
      }
  }

  int guyAnimsCount = 0;
  ModelAnimation *guyAnims =
      LoadModelAnimations("game/models/monika_walk.glb", &guyAnimsCount);

  Mesh cubeMesh = GenMeshCube(2.0f, 2.0f, 2.0f);
  Model cubeModel = LoadModelFromMesh(cubeMesh);

  Mesh floorMesh = GenMeshCube(2.0f, 1.0f, 2.0f);
  Model floorModel = LoadModelFromMesh(floorMesh);

  RenderTexture2D target = LoadRenderTexture(render_width, render_height);
  RenderTexture2D final_target = LoadRenderTexture(render_width, render_height);

  Shader lighting_shader = LoadShader(0, "game/core/lighting.fs");
  int tod_loc = GetShaderLocation(lighting_shader, "time_of_day");
  int lq_loc = GetShaderLocation(lighting_shader, "lighting_quality");
  int sh_loc = GetShaderLocation(lighting_shader, "shadows_enabled");
  
  int time_loc = GetShaderLocation(lighting_shader, "u_time");
  int bloom_loc = GetShaderLocation(lighting_shader, "u_bloom");
  int clouds_loc = GetShaderLocation(lighting_shader, "u_clouds");
  int weather_loc = GetShaderLocation(lighting_shader, "u_rain_intensity");
  int yaw_loc = GetShaderLocation(lighting_shader, "u_yaw");
  int pitch_loc = GetShaderLocation(lighting_shader, "u_pitch");
  int aspect_loc = GetShaderLocation(lighting_shader, "u_aspect");
  int cam_pos_loc = GetShaderLocation(lighting_shader, "u_cam_pos");
  int fov_loc = GetShaderLocation(lighting_shader, "u_fovy");
  int flash_loc = GetShaderLocation(lighting_shader, "u_flash_intensity");
  int fl_loc = GetShaderLocation(lighting_shader, "u_flashlight_active");
  int fl_offset_loc = GetShaderLocation(lighting_shader, "u_flashlight_offset");
  int mb_strength_loc = GetShaderLocation(lighting_shader, "u_motion_blur_strength");
  int cam_vel_loc = GetShaderLocation(lighting_shader, "u_cam_velocity");

  Shader cel_shader = LoadShader("game/core/cel_model.vs", "game/core/cel_model.fs");
  int cel_light_dir_loc = GetShaderLocation(cel_shader, "u_light_dir");
  int cel_light_col_loc = GetShaderLocation(cel_shader, "u_light_color");
  int cel_amb_col_loc   = GetShaderLocation(cel_shader, "u_ambient_color");
  int cel_view_pos_loc  = GetShaderLocation(cel_shader, "u_view_pos");
  int cel_rim_pow_loc   = GetShaderLocation(cel_shader, "u_rim_power");
  int cel_rim_col_loc   = GetShaderLocation(cel_shader, "u_rim_color");

  Shader dyn_light_shader = LoadShader("game/core/dynamic_lighting.vs", "game/core/dynamic_lighting.fs");
  int dyn_view_pos_loc  = GetShaderLocation(dyn_light_shader, "u_view_pos");
  int dyn_sun_dir_loc   = GetShaderLocation(dyn_light_shader, "u_sun_dir");
  int dyn_sun_col_loc   = GetShaderLocation(dyn_light_shader, "u_sun_color");
  int dyn_amb_col_loc   = GetShaderLocation(dyn_light_shader, "u_ambient_color");
  int dyn_fl_active_loc = GetShaderLocation(dyn_light_shader, "u_flashlight_active");
  int dyn_fl_pos_loc    = GetShaderLocation(dyn_light_shader, "u_flashlight_pos");
  int dyn_fl_dir_loc    = GetShaderLocation(dyn_light_shader, "u_flashlight_dir");
  int dyn_fl_col_loc    = GetShaderLocation(dyn_light_shader, "u_flashlight_color");
  int dyn_muz_int_loc   = GetShaderLocation(dyn_light_shader, "u_muzzle_flash_intensity");
  int dyn_muz_pos_loc   = GetShaderLocation(dyn_light_shader, "u_muzzle_flash_pos");
  int dyn_muz_col_loc   = GetShaderLocation(dyn_light_shader, "u_muzzle_flash_color");
  int dyn_flat_norm_loc = GetShaderLocation(dyn_light_shader, "u_use_flat_normals");

  for (int m = 0; m < guyModel.materialCount; m++) {
    Texture2D *tex = &guyModel.materials[m].maps[MATERIAL_MAP_DIFFUSE].texture;
    if (tex->id > 1) {
      GenTextureMipmaps(tex);
      SetTextureFilter(*tex, TEXTURE_FILTER_ANISOTROPIC_8X);
    }
  }

  Model world3DModel = {0};
  bool has_world3d_model = false;
  BoundingBox *world3d_mesh_boxes = NULL;

  const char *model_path = NULL;
  float grid_cell_size = 2.0f;

  if (is_arena_mode == 3) {
    if (FileExists("game/models/minecraft-city.glb")) {
      model_path = "game/models/minecraft-city.glb";
    }
    grid_cell_size = 4.0f;
  } else if (is_arena_mode == 2) {
    if (FileExists("game/models/construction-school.glb")) {
      model_path = "game/models/construction-school.glb";
    }
    grid_cell_size = 2.0f;
  }

  if (model_path != NULL) {
    world3DModel = LoadModel(model_path);
    has_world3d_model = true;
    InitWorldGrid(world3DModel, grid_cell_size);
    world3d_mesh_boxes = (BoundingBox *)malloc(world3DModel.meshCount * sizeof(BoundingBox));
    for (int m = 0; m < world3DModel.meshCount; m++) {
      world3d_mesh_boxes[m] = GetMeshBoundingBox(world3DModel.meshes[m]);
    }
    for (int m = 0; m < world3DModel.materialCount; m++) {
      Texture2D *tex = &world3DModel.materials[m].maps[MATERIAL_MAP_DIFFUSE].texture;
      if (tex->id > 1) {
        GenTextureMipmaps(tex);
        SetTextureFilter(*tex, TEXTURE_FILTER_ANISOTROPIC_8X);
      }
    }
  }

  Model sayoriModel = {0};
  bool has_sayori_model = false;
  ModelAnimation sayori_anim = {0};
  int s_hip_l = -1, s_knee_l = -1, s_ankle_l = -1, s_toe_l = -1;
  int s_hip_r = -1, s_knee_r = -1, s_ankle_r = -1, s_toe_r = -1;
  int s_arm_l = -1, s_arm_r = -1;
  int s_lower_body = -1, s_upper_body = -1, s_upper_body2 = -1, s_neck = -1, s_head = -1;
  SayoriArmChainIdx s_chain_l = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
  SayoriHandFingers s_fingers_l = { { -1, -1, -1 }, { -1, -1, -1 }, { -1, -1, -1 }, { -1, -1, -1 }, { -1, -1, -1 } };
  SayoriHandFingers s_fingers_r = { { -1, -1, -1 }, { -1, -1, -1 }, { -1, -1, -1 }, { -1, -1, -1 }, { -1, -1, -1 } };
  SayoriArmChainIdx s_chain_r = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
  int s_skirt_bones[11][10];
  for (int c = 0; c < 11; c++) {
    for (int l = 0; l < 10; l++) s_skirt_bones[c][l] = -1;
  }
  static Vector3 skirt_particles_pos[11][10];
  static Vector3 skirt_particles_prev[11][10];
  static float skirt_vert_len[11][10];
  static float skirt_horiz_len[11][10];
  static bool skirt_cloth_inited = false;

  if (FileExists("game/models/sayori.glb")) {
    sayoriModel = LoadModel("game/models/sayori.glb");
    has_sayori_model = true;
    for (int m = 0; m < sayoriModel.materialCount; m++) {
      Texture2D *tex = &sayoriModel.materials[m].maps[MATERIAL_MAP_DIFFUSE].texture;
      if (tex->id > 1) {
        GenTextureMipmaps(tex);
        SetTextureFilter(*tex, TEXTURE_FILTER_ANISOTROPIC_8X);
      }
    }

    for (int b = 0; b < sayoriModel.skeleton.boneCount; b++) {
      const char *bname = sayoriModel.skeleton.bones[b].name;
      if (strcmp(bname, "LegD_L") == 0) s_hip_l = b;
      else if (strcmp(bname, "KneeD_L") == 0) s_knee_l = b;
      else if (strcmp(bname, "AnkleD_L") == 0) s_ankle_l = b;
      else if (strcmp(bname, "LegTipEX_L") == 0) s_toe_l = b;
      else if (strcmp(bname, "LegD_R") == 0) s_hip_r = b;
      else if (strcmp(bname, "KneeD_R") == 0) s_knee_r = b;
      else if (strcmp(bname, "AnkleD_R") == 0) s_ankle_r = b;
      else if (strcmp(bname, "LegTipEX_R") == 0) s_toe_r = b;
      else if (strcmp(bname, "Shoulder_L") == 0) s_chain_l.sh = b;
      else if (strcmp(bname, "ShoulderC_L") == 0) s_chain_l.sh_c = b;
      else if (strcmp(bname, "Arm_L") == 0) { s_arm_l = b; s_chain_l.arm = b; }
      else if (strcmp(bname, "ArmTwist1_L") == 0) s_chain_l.arm_tw1 = b;
      else if (strcmp(bname, "ArmTwist_L") == 0) s_chain_l.arm_tw = b;
      else if (strcmp(bname, "ArmTwist3_L") == 0) s_chain_l.arm_tw3 = b;
      else if (strcmp(bname, "Elbow_L") == 0) s_chain_l.elbow = b;
      else if (strcmp(bname, "HandTwist1_L") == 0) s_chain_l.hand_tw1 = b;
      else if (strcmp(bname, "HandTwist2_L") == 0) s_chain_l.hand_tw2 = b;
      else if (strcmp(bname, "HandTwist_L") == 0) s_chain_l.hand_tw = b;
      else if (strcmp(bname, "HandTwist3_L") == 0) s_chain_l.hand_tw3 = b;
      else if (strcmp(bname, "Wrist_L") == 0) s_chain_l.wrist = b;
      else if (strcmp(bname, "Shoulder_R") == 0) s_chain_r.sh = b;
      else if (strcmp(bname, "ShoulderC_R") == 0) s_chain_r.sh_c = b;
      else if (strcmp(bname, "Arm_R") == 0) { s_arm_r = b; s_chain_r.arm = b; }
      else if (strcmp(bname, "ArmTwist1_R") == 0) s_chain_r.arm_tw1 = b;
      else if (strcmp(bname, "ArmTwist_R") == 0) s_chain_r.arm_tw = b;
      else if (strcmp(bname, "ArmTwist3_R") == 0) s_chain_r.arm_tw3 = b;
      else if (strcmp(bname, "Elbow_R") == 0) s_chain_r.elbow = b;
      else if (strcmp(bname, "HandTwist1_R") == 0) s_chain_r.hand_tw1 = b;
      else if (strcmp(bname, "HandTwist2_R") == 0) s_chain_r.hand_tw2 = b;
      else if (strcmp(bname, "HandTwist_R") == 0) s_chain_r.hand_tw = b;
      else if (strcmp(bname, "HandTwist3_R") == 0) s_chain_r.hand_tw3 = b;
      else if (strcmp(bname, "Wrist_R") == 0) s_chain_r.wrist = b;
      else if (strcmp(bname, "Thumb0_L") == 0) s_fingers_l.thumb[0] = b;
      else if (strcmp(bname, "Thumb1_L") == 0) s_fingers_l.thumb[1] = b;
      else if (strcmp(bname, "Thumb2_L") == 0) s_fingers_l.thumb[2] = b;
      else if (strcmp(bname, "IndexFinger1_L") == 0) s_fingers_l.index[0] = b;
      else if (strcmp(bname, "IndexFinger2_L") == 0) s_fingers_l.index[1] = b;
      else if (strcmp(bname, "IndexFinger3_L") == 0) s_fingers_l.index[2] = b;
      else if (strcmp(bname, "MiddleFinger1_L") == 0) s_fingers_l.middle[0] = b;
      else if (strcmp(bname, "MiddleFinger2_L") == 0) s_fingers_l.middle[1] = b;
      else if (strcmp(bname, "MiddleFinger3_L") == 0) s_fingers_l.middle[2] = b;
      else if (strcmp(bname, "RingFinger1_L") == 0) s_fingers_l.ring[0] = b;
      else if (strcmp(bname, "RingFinger2_L") == 0) s_fingers_l.ring[1] = b;
      else if (strcmp(bname, "RingFinger3_L") == 0) s_fingers_l.ring[2] = b;
      else if (strcmp(bname, "LittleFinger1_L") == 0) s_fingers_l.little[0] = b;
      else if (strcmp(bname, "LittleFinger2_L") == 0) s_fingers_l.little[1] = b;
      else if (strcmp(bname, "LittleFinger3_L") == 0) s_fingers_l.little[2] = b;

      else if (strcmp(bname, "Thumb0_R") == 0) s_fingers_r.thumb[0] = b;
      else if (strcmp(bname, "Thumb1_R") == 0) s_fingers_r.thumb[1] = b;
      else if (strcmp(bname, "Thumb2_R") == 0) s_fingers_r.thumb[2] = b;
      else if (strcmp(bname, "IndexFinger1_R") == 0) s_fingers_r.index[0] = b;
      else if (strcmp(bname, "IndexFinger2_R") == 0) s_fingers_r.index[1] = b;
      else if (strcmp(bname, "IndexFinger3_R") == 0) s_fingers_r.index[2] = b;
      else if (strcmp(bname, "MiddleFinger1_R") == 0) s_fingers_r.middle[0] = b;
      else if (strcmp(bname, "MiddleFinger2_R") == 0) s_fingers_r.middle[1] = b;
      else if (strcmp(bname, "MiddleFinger3_R") == 0) s_fingers_r.middle[2] = b;
      else if (strcmp(bname, "RingFinger1_R") == 0) s_fingers_r.ring[0] = b;
      else if (strcmp(bname, "RingFinger2_R") == 0) s_fingers_r.ring[1] = b;
      else if (strcmp(bname, "RingFinger3_R") == 0) s_fingers_r.ring[2] = b;
      else if (strcmp(bname, "LittleFinger1_R") == 0) s_fingers_r.little[0] = b;
      else if (strcmp(bname, "LittleFinger2_R") == 0) s_fingers_r.little[1] = b;
      else if (strcmp(bname, "LittleFinger3_R") == 0) s_fingers_r.little[2] = b;
      else if (strcmp(bname, "LowerBody") == 0) s_lower_body = b;
      else if (strcmp(bname, "UpperBody") == 0) s_upper_body = b;
      else if (strcmp(bname, "UpperBody2") == 0) s_upper_body2 = b;
      else if (strcmp(bname, "Neck") == 0) s_neck = b;
      else if (strcmp(bname, "Head") == 0) s_head = b;
      else if (strncmp(bname, "Skirt_", 6) == 0) {
        int link = 0, col = 0;
        if (sscanf(bname, "Skirt_%d_%d", &link, &col) == 2) {
          if (link >= 0 && link < 10 && col >= 0 && col < 11) {
            s_skirt_bones[col][link] = b;
          }
        }
      }
    }

    sayori_anim.boneCount = sayoriModel.skeleton.boneCount;
    sayori_anim.keyframeCount = 1;
    sayori_anim.keyframePoses = (ModelAnimPose *)malloc(sizeof(ModelAnimPose));
    if (sayori_anim.keyframePoses != NULL) {
      sayori_anim.keyframePoses[0] = (Transform *)malloc(sayori_anim.boneCount * sizeof(Transform));
      for (int b = 0; b < (int)sayori_anim.boneCount; b++) {
        sayori_anim.keyframePoses[0][b] = sayoriModel.skeleton.bindPose[b];
      }
      if (s_arm_l >= 0) sayori_anim.keyframePoses[0][s_arm_l].rotation = QuaternionMultiply(sayori_anim.keyframePoses[0][s_arm_l].rotation, QuaternionFromAxisAngle((Vector3){0, 0, 1}, -70.0f * DEG2RAD));
      if (s_arm_r >= 0) sayori_anim.keyframePoses[0][s_arm_r].rotation = QuaternionMultiply(sayori_anim.keyframePoses[0][s_arm_r].rotation, QuaternionFromAxisAngle((Vector3){0, 0, 1}, 70.0f * DEG2RAD));
      UpdateModelAnimation(sayoriModel, sayori_anim, 0);
    }

    if (!skirt_cloth_inited) {
      for (int c = 0; c < 11; c++) {
        for (int l = 0; l < 10; l++) {
          int b = s_skirt_bones[c][l];
          if (b >= 0) {
            Vector3 bp = sayoriModel.skeleton.bindPose[b].translation;
            skirt_particles_pos[c][l] = bp;
            skirt_particles_prev[c][l] = bp;
            if (l > 0) {
              int pb = s_skirt_bones[c][l - 1];
              if (pb >= 0) skirt_vert_len[c][l] = Vector3Distance(bp, sayoriModel.skeleton.bindPose[pb].translation);
            } else {
              skirt_vert_len[c][l] = 0.0f;
            }
          }
        }
      }
      for (int l = 0; l < 10; l++) {
        for (int c = 0; c < 11; c++) {
          int c_next = (c + 1) % 11;
          int b1 = s_skirt_bones[c][l];
          int b2 = s_skirt_bones[c_next][l];
          if (b1 >= 0 && b2 >= 0) {
            skirt_horiz_len[c][l] = Vector3Distance(sayoriModel.skeleton.bindPose[b1].translation,
                                                    sayoriModel.skeleton.bindPose[b2].translation);
          }
        }
      }
      skirt_cloth_inited = true;
    }
  }

  for (int i = 0; i < 4; i++) {
    for (int m = 0; m < weaponModels[i].materialCount; m++) {
      Texture2D *tex = &weaponModels[i].materials[m].maps[MATERIAL_MAP_DIFFUSE].texture;
      if (tex->id > 1) {
        GenTextureMipmaps(tex);
        SetTextureFilter(*tex, TEXTURE_FILTER_ANISOTROPIC_8X);
      }
    }
  }

  for (int m = 0; m < cubeModel.materialCount; m++) {
    cubeModel.materials[m].shader = dyn_light_shader;
  }
  for (int m = 0; m < floorModel.materialCount; m++) {
    floorModel.materials[m].shader = dyn_light_shader;
  }
  for (int m = 0; m < guyModel.materialCount; m++) {
    guyModel.materials[m].shader = dyn_light_shader;
  }
  for (int i = 0; i < 4; i++) {
    for (int m = 0; m < weaponModels[i].materialCount; m++) {
      weaponModels[i].materials[m].shader = dyn_light_shader;
    }
  }
  if (has_world3d_model) {
    for (int m = 0; m < world3DModel.materialCount; m++) {
      world3DModel.materials[m].shader = dyn_light_shader;
    }
  }
  if (has_sayori_model) {
    for (int m = 0; m < sayoriModel.materialCount; m++) {
      sayoriModel.materials[m].shader = dyn_light_shader;
    }
  }

  bool enable_cel_shading = false;
  if (enable_cel_shading) {
    for (int m = 0; m < guyModel.materialCount; m++) {
      guyModel.materials[m].shader = cel_shader;
    }
    for (int i = 0; i < 4; i++) {
      for (int m = 0; m < weaponModels[i].materialCount; m++) {
        weaponModels[i].materials[m].shader = cel_shader;
      }
    }
    if (has_world3d_model) {
      for (int m = 0; m < world3DModel.materialCount; m++) {
        world3DModel.materials[m].shader = cel_shader;
      }
    }
    if (has_sayori_model) {
      for (int m = 0; m < sayoriModel.materialCount; m++) {
        sayoriModel.materials[m].shader = cel_shader;
      }
    }
  }

  Camera3D camera = {0};
  camera.up = (Vector3){0.0f, 1.0f, 0.0f};
  camera.fovy = 60.0f;
  camera.projection = CAMERA_PERSPECTIVE;

  pthread_t thread_id;
  pthread_create(&thread_id, NULL, input_listener, NULL);

  float arena_timer = 0.0f;
  int active_enemies = 0;
  float muzzle_flash_timer = 0.0f;
  float ads_lerp = 0.0f;
  float gait_cycle = 0.0f;
  float gait_weight = 0.0f;
  float prev_player_x = current_cam.x;
  float prev_player_z = current_cam.z;
  float pelvis_yaw = current_cam.yaw;
  bool pelvis_yaw_inited = false;

  while (!WindowShouldClose()) {
    pthread_mutex_lock(&input_mutex);
    float mdx = current_input.mouse_dx;
    float mdy = current_input.mouse_dy;
    int k_w = current_input.w | current_input.up;
    int k_s = current_input.s | current_input.down;
    int k_a = current_input.a | current_input.left;
    int k_d = current_input.d | current_input.right;
    int k_space = current_input.space;
    int k_shift = current_input.shift;
    int k_ctrl = current_input.ctrl;
    int k_shoot = current_input.shoot;
    int is_aiming = current_input.is_aiming;

    current_input.mouse_dx = 0.0f;
    current_input.mouse_dy = 0.0f;
    pthread_mutex_unlock(&input_mutex);

    float dt = GetFrameTime();
    if (dt > 0.1f)
      dt = 0.1f;

    if (muzzle_flash_timer > 0.0f)
      muzzle_flash_timer -= dt;

    int cur_wp = current_input.active_weapon;
    if (cur_wp < 0 || cur_wp >= 4) cur_wp = 1;

    ads_lerp = Lerp(ads_lerp, is_aiming ? 1.0f : 0.0f, fminf(1.0f, 13.5f * dt));
    static SpringDamper1D ads_rot_spring = {0};
    ads_rot_spring.target = is_aiming ? 1.0f : 0.0f;
    UpdateSpringDamper(&ads_rot_spring, 240.0f, 26.0f, dt);

    static float shoot_cooldown = 0.0f;
    static float recoil_offset_z = 0.0f;
    static float recoil_pitch = 0.0f;

    static SpringDamper1D lag_yaw = {0};
    static SpringDamper1D lag_pitch = {0};
    static SpringDamper1D lag_roll = {0};
    static SpringDamper1D lag_trans_x = {0};
    static SpringDamper1D lag_trans_y = {0};
    static SpringDamper1D recoil_pitch_spring = {0};
    static SpringDamper1D recoil_z_spring = {0};
    static SpringDamper1D recoil_roll_spring = {0};
    static SpringDamper1D recoil_yaw_spring = {0};
    static SpringDamper1D cam_recoil_pitch_spring = {0};
    static SpringDamper1D wall_lift_spring = {0};
    static SpringDamper1D heel_shock_spring = {0};
    static float slide_blowback = 0.0f;
    static float trigger_pull = 0.0f;
    static float crouch_lerp = 0.0f;

    static SpringDamper1D support_shear_z = {0};
    static SpringDamper1D support_shear_yaw = {0};

    static SpringDamper1D inertia_elbow_flex = {0};
    static float sprint_sul_t = 0.0f;

    static float trigger_travel = 0.0f;
    static bool trigger_sear_broken = false;
    static bool trigger_has_reset = true;

    static float heart_rate_bpm = 72.0f;
    static float cardio_phase = 0.0f;
    static float ads_steady_timer = 0.0f;
    static float breath_pause_factor = 1.0f;
    static float hypoxia_tremor_pitch = 0.0f;
    static float hypoxia_tremor_yaw   = 0.0f;
    static float tremor_time = 0.0f;

    if (shoot_cooldown > 0.0f) {
      shoot_cooldown -= dt;
      if (cur_wp == 1 && shoot_cooldown <= weapons[1].cooldown * 0.35f && !trigger_has_reset) {
        trigger_has_reset = true;
      }
    }

    recoil_offset_z = Lerp(recoil_offset_z, 0.0f, 15.0f * dt);
    recoil_pitch = Lerp(recoil_pitch, 0.0f, 15.0f * dt);

    float free_aim_scale = (1.0f - ads_lerp * 0.82f);
    lag_yaw.target = Clamp(-mdx * 0.40f * free_aim_scale, -12.0f, 12.0f);
    lag_pitch.target = Clamp(-mdy * 0.40f * free_aim_scale, -10.0f, 10.0f);
    lag_roll.target = Clamp(-mdx * 0.55f * free_aim_scale, -18.0f, 18.0f);
    lag_trans_x.target = Clamp(-mdx * 0.0016f * free_aim_scale, -0.020f, 0.020f);
    lag_trans_y.target = Clamp(-mdy * 0.0016f * free_aim_scale, -0.016f, 0.016f);

    float stiffness = Lerp(155.0f, 400.0f, ads_lerp);
    float damping = Lerp(19.0f, 34.0f, ads_lerp);

    UpdateSpringDamper(&lag_yaw, stiffness, damping, dt);
    UpdateSpringDamper(&lag_pitch, stiffness, damping, dt);
    UpdateSpringDamper(&lag_roll, stiffness, damping, dt);
    UpdateSpringDamper(&lag_trans_x, stiffness, damping, dt);
    UpdateSpringDamper(&lag_trans_y, stiffness, damping, dt);

    slide_blowback = Lerp(slide_blowback, 0.0f, fminf(1.0f, 28.0f * dt));
    trigger_pull = Lerp(trigger_pull, 0.0f, fminf(1.0f, 18.0f * dt));
    recoil_pitch_spring.target = 0.0f;
    recoil_z_spring.target = 0.0f;
    recoil_roll_spring.target = 0.0f;
    recoil_yaw_spring.target = 0.0f;
    cam_recoil_pitch_spring.target = 0.0f;
    heel_shock_spring.target = 0.0f;

    UpdateSpringDamper(&recoil_pitch_spring, 540.0f, 38.0f, dt);
    UpdateSpringDamper(&recoil_z_spring, 520.0f, 36.0f, dt);
    UpdateSpringDamper(&recoil_roll_spring, 540.0f, 38.0f, dt);
    UpdateSpringDamper(&recoil_yaw_spring, 540.0f, 38.0f, dt);
    UpdateSpringDamper(&cam_recoil_pitch_spring, 640.0f, 44.0f, dt);
    UpdateSpringDamper(&heel_shock_spring, 210.0f, 20.0f, dt);

    support_shear_z.target = 0.0f;
    support_shear_yaw.target = 0.0f;
    UpdateSpringDamper(&support_shear_z, 620.0f, 42.0f, dt);
    UpdateSpringDamper(&support_shear_yaw, 620.0f, 42.0f, dt);

    float snap_speed = sqrtf(mdx * mdx + mdy * mdy);
    float flex_scale = is_aiming ? 0.0006f : 0.0014f;
    inertia_elbow_flex.target = Clamp(snap_speed * flex_scale, 0.0f, 0.032f);
    float flex_stiffness = Lerp(180.0f, 420.0f, ads_lerp);
    float flex_damping   = Lerp(20.0f, 36.0f, ads_lerp);
    UpdateSpringDamper(&inertia_elbow_flex, flex_stiffness, flex_damping, dt);

    if (cur_wp == 1) {
      if (k_shoot) {
        if (!trigger_sear_broken && trigger_has_reset) {
          if (trigger_travel < 0.38f) {
            trigger_travel += 20.0f * dt;
          } else if (trigger_travel < 0.75f) {
            trigger_travel += 12.0f * dt;
          } else {
            trigger_travel = 1.0f;
          }
        } else {
          trigger_travel = 1.0f;
        }
      } else {
        trigger_travel = fmaxf(0.0f, trigger_travel - 14.0f * dt);
        if (trigger_travel <= 0.42f) {
          trigger_sear_broken = false;
          trigger_has_reset = true;
        }
      }
    } else {
      trigger_travel = k_shoot ? 1.0f : 0.0f;
    }

    current_cam.yaw += mdx;
    current_cam.pitch += mdy;

    if (current_cam.pitch > 89.0f)
      current_cam.pitch = 89.0f;
    if (current_cam.pitch < -89.0f)
      current_cam.pitch = -89.0f;

    float phys_yawRad = current_cam.yaw * DEG2RAD;
    float phys_pitchRad = current_cam.pitch * DEG2RAD;

    Vector3 forward;
    forward.x = cosf(phys_pitchRad) * cosf(phys_yawRad);
    forward.y = sinf(phys_pitchRad);
    forward.z = cosf(phys_pitchRad) * sinf(phys_yawRad);

    Vector3 p_pos = {camera.position.x, camera.position.y, camera.position.z};

    for (int i = 0; i < num_enemies; i++) {
      if (enemies[i].state >= 3)
        continue;

      enemies[i].timer -= dt;
      enemies[i].reload_timer -= dt;
      enemies[i].dodge_timer -= dt;

      Vector3 e_pos = {enemies[i].x, enemies[i].y + 1.0f, enemies[i].z};
      float dx = p_pos.x - e_pos.x;
      float dz = p_pos.z - e_pos.z;
      float dist = sqrtf(dx * dx + dz * dz);

      int has_los = HasLineOfSight(e_pos, p_pos);
      int should_move = 1;

      if (enemies[i].type == 1 || enemies[i].type == 9) { // yuritler
        if (dist <= 20.0f && has_los) {
          if (enemies[i].timer <= 0.0f) {
            float base_angle = atan2f(p_pos.z - e_pos.z, p_pos.x - e_pos.x);
            for (int f = 0; f < 4; f++) {
              float angle = base_angle + (f - 1.5f) * 0.12f;
              Vector3 dir = {cosf(angle), (p_pos.y - e_pos.y) / (dist > 0.1f ? dist : 1.0f), sinf(angle)};
              SpawnProjectile(e_pos, dir, 13.0f, 5.0f, 6, 0);
            }
            enemies[i].timer = 1.0f;
          }
          if (dist < 7.0f) should_move = 0;
        }
      } else if (enemies[i].type == 2) { // elite enemy
        if (enemies[i].reload_timer > 0.0f) {
        } else if (dist <= 16.0f && has_los) {
          if (enemies[i].timer <= 0.0f) {
            Vector3 dir = Vector3Normalize(Vector3Subtract(p_pos, e_pos));
            SpawnProjectile(e_pos, dir, 16.0f, 4.0f, 6, 0);
            enemies[i].timer = 0.1f;
            enemies[i].burst_count++;
            if (enemies[i].burst_count >= 10) {
              enemies[i].burst_count = 0;
              enemies[i].reload_timer = 5.0f;
            }
          }
          if (dist < 6.0f) should_move = 0;
        }
      } else if (enemies[i].type == 3) { // sniper
        if (dist <= 28.0f && has_los) {
          if (enemies[i].timer <= 0.0f) {
            Vector3 dir = Vector3Normalize(Vector3Subtract(p_pos, e_pos));
            SpawnProjectile(e_pos, dir, 25.0f, 20.0f, 14, 0);
            enemies[i].timer = 1.8f;
          }
          if (dist < 10.0f) should_move = 0;
        }
      } else { // standard enemy
        if (dist <= 15.0f && has_los) {
          if (enemies[i].timer <= 0.0f) {
            Vector3 dir = Vector3Normalize(Vector3Subtract(p_pos, e_pos));
            SpawnProjectile(e_pos, dir, 14.0f, 10.0f, 6, 0);
            enemies[i].timer = 1.5f;
          }
          if (dist < 5.0f) should_move = 0;
        }
      }

      if (should_move && dist > 0.1f) {
        float dir_x = dx / dist;
        float dir_z = dz / dist;
        float ms = enemies[i].move_speed;
        float next_x = enemies[i].x + dir_x * ms * dt;
        float next_z = enemies[i].z + dir_z * ms * dt;

        int ex_round = (int)roundf(enemies[i].x / 2.0f);
        int ez_round = (int)roundf(enemies[i].z / 2.0f);
        float e_max_z = -1000.0f;
        for (int l = 0; l < map_layers; l++) {
          if (GetBlock(ex_round, l, ez_round) > 0) {
            float top_z = (l * 2.0f) + 2.0f;
            if (top_z > e_max_z && top_z <= enemies[i].y + 1.5f) {
              e_max_z = top_z;
            }
          }
        }
        if (e_max_z == -1000.0f)
          e_max_z = 1.0f;
        enemies[i].y = e_max_z;

        int el = (int)floorf(enemies[i].y / 2.0f);

        int ex_block = GetBlock((int)roundf(next_x / 2.0f), el, (int)roundf(enemies[i].z / 2.0f));
        if (ex_block <= 0)
          enemies[i].x = next_x;

        int ez_block = GetBlock((int)roundf(enemies[i].x / 2.0f), el, (int)roundf(next_z / 2.0f));
        if (ez_block <= 0)
          enemies[i].z = next_z;
      }
    }

    active_enemies = 0;
    for (int i = 0; i < num_enemies; i++) {
      if (enemies[i].state < 3)
        active_enemies++;
    }

    if (is_arena_mode == 1) {
      total_arena_time += dt;
      current_cam.time_of_day = fmodf(12.0f + total_arena_time * 0.04f, 24.0f);

      weather_timer -= dt * 0.04f;
      if (weather_timer <= 0.0f) {
        if (weather_active) {
          weather_active = 0;
          weather_timer = 5.0f;
        } else {
          if (rand() % 100 < 10) {
            weather_active = 1;
            weather_timer = 6.0f;
          } else {
            weather_timer = 5.0f;
          }
        }
      }
      if (weather_active) {
        current_input.rain_intensity = Lerp(current_input.rain_intensity, 1.0f, 0.5f * dt);
      } else {
        current_input.rain_intensity = Lerp(current_input.rain_intensity, 0.0f, 0.5f * dt);
      }

      if (num_spawn_points > 0) {
        if (active_enemies == 0) {
          if (inter_round_timer > 0.0f) {
            inter_round_timer -= dt;
            if (inter_round_timer <= 0.0f) {
              StartNextArenaRound();
            }
          } else {
            inter_round_timer = 10.0f;
            SendEvent(31, 10);
          }
        }
      }
    }

    int px = (int)roundf(current_cam.x / 2.0f);
    int pz = (int)roundf(current_cam.z / 2.0f);

    float max_z = -1000.0f;
    if (is_arena_mode >= 2) {
      max_z = GetGroundHeight(current_cam.x, current_cam.z, current_cam.y, 0.45f);
    } else {
      for (int l = 0; l < map_layers; l++) {
        if (GetBlock(px, l, pz) > 0) {
          float top_z = (l * 2.0f) + 2.0f;
          if (top_z > max_z && top_z <= current_cam.y + 1.5f) {
            max_z = top_z;
          }
        }
      }
      if (max_z == -1000.0f) {
        max_z = 1.0f;
      }
    }

    if (current_cam.is_grounded == 0) {
      float grav = (is_arena_mode >= 2) ? 22.0f : 35.0f;
      current_cam.vel_y -= grav * dt;
      current_cam.y += current_cam.vel_y * dt;

      if (current_cam.y <= max_z) {
        float land_vel = fabsf(current_cam.vel_y);
        current_cam.y = max_z;
        current_cam.vel_y = 0.0f;
        current_cam.is_grounded = 1;
        if (land_vel > 1.2f) {
          heel_shock_spring.vel -= Clamp(land_vel * 0.065f, 0.10f, 0.55f);
        }
      }
    } else {
      float drop_thresh = (is_arena_mode >= 2) ? 0.40f : 0.15f;
      if (current_cam.y > max_z + drop_thresh) {
        current_cam.is_grounded = 0;
      } else {
        current_cam.y = max_z;
        if (k_space) {
          current_cam.vel_y = (is_arena_mode >= 2) ? 7.5f : 15.0f;
          current_cam.is_grounded = 0;
        }
      }
    }

    if (is_arena_mode >= 2 && current_cam.y < -30.0f) {
      current_cam.x = (is_arena_mode == 3) ? 290.0f : 0.0f;
      current_cam.y = (is_arena_mode == 3) ? 25.0f : 0.0f;
      current_cam.z = (is_arena_mode == 3) ? 316.0f : 24.0f;
      current_cam.vel_y = 0.0f;
      current_cam.is_grounded = 1;
    }

    if (is_arena_mode >= 2) {
      if (current_input.up) {
        current_cam.y += 8.0f * dt;
        current_cam.vel_y = 0.0f;
        current_cam.is_grounded = 1;
      }
      if (current_input.down) {
        current_cam.y = fmaxf(-10.0f, current_cam.y - 8.0f * dt);
        current_cam.vel_y = 0.0f;
        current_cam.is_grounded = (current_cam.y <= max_z + 0.1f);
      }
    }

    float base_speed = (is_arena_mode >= 2 ? (is_arena_mode == 3 ? 5.0f : 3.8f) : 3.5f);
    if (k_shift)
      base_speed *= (is_arena_mode == 3 ? 1.8f : 1.65f);

    float base_eye = (is_arena_mode >= 2) ? 1.435f : 1.0f;
    float crouch_offset = 0.38f;
    bool wants_crouch = (k_ctrl && current_cam.is_grounded);
    crouch_lerp = Lerp(crouch_lerp, wants_crouch ? 1.0f : 0.0f, fminf(1.0f, 14.0f * dt));

    static float smoothed_cam_y = -9999.0f;
    if (smoothed_cam_y < -500.0f) smoothed_cam_y = current_cam.y;
    smoothed_cam_y = Lerp(smoothed_cam_y, current_cam.y, fminf(1.0f, 22.0f * dt));

    float crouch_drop = crouch_lerp * crouch_offset;
    float target_height = smoothed_cam_y + base_eye - crouch_drop;
    if (wants_crouch) {
      base_speed *= 0.5f;
    }

    camera.position = (Vector3){current_cam.x, target_height, current_cam.z};

    float fx = cosf(phys_yawRad);
    float fz = sinf(phys_yawRad);
    float rx = cosf(phys_yawRad + PI / 2.0f);
    float rz = sinf(phys_yawRad + PI / 2.0f);

    float wish_x = 0.0f;
    float wish_z = 0.0f;
    if (k_w) { wish_x += fx; wish_z += fz; }
    if (k_s) { wish_x -= fx; wish_z -= fz; }
    if (k_a) { wish_x -= rx; wish_z -= rz; }
    if (k_d) { wish_x += rx; wish_z += rz; }

    float wish_len = sqrtf(wish_x * wish_x + wish_z * wish_z);
    if (wish_len > 1.0f) {
      wish_x /= wish_len;
      wish_z /= wish_len;
      wish_len = 1.0f;
    }

    static float player_vx = 0.0f;
    static float player_vz = 0.0f;
    static bool was_sprint_active = false;
    static float stair_grade = 0.0f;

    float target_stair_grade = 0.0f;
    if (world_grid.is_initialized && current_cam.is_grounded) {
      float cur_vlen = sqrtf(player_vx * player_vx + player_vz * player_vz);
      if (wish_len > 0.01f || cur_vlen > 0.20f) {
        float p_dir_x = (wish_len > 0.01f) ? wish_x : (player_vx / (cur_vlen > 1e-4f ? cur_vlen : 1.0f));
        float p_dir_z = (wish_len > 0.01f) ? wish_z : (player_vz / (cur_vlen > 1e-4f ? cur_vlen : 1.0f));
        float probe_x = p_dir_x * 0.28f;
        float probe_z = p_dir_z * 0.28f;
        float g_ahead = GetGroundHeight(current_cam.x + probe_x, current_cam.z + probe_z, current_cam.y, 0.55f);
        float g_cur   = GetGroundHeight(current_cam.x, current_cam.z, current_cam.y, 0.55f);
        if (g_ahead > -500.0f && g_cur > -500.0f) {
          float dy_step = g_ahead - g_cur;
          if (fabsf(dy_step) > 0.035f && fabsf(dy_step) < 0.65f) {
            target_stair_grade = Clamp(dy_step / 0.18f, -1.0f, 1.0f);
          }
        }
      } else {
        float pel_rad = (90.0f - pelvis_yaw) * DEG2RAD;
        float p_fx = sinf(pel_rad);
        float p_fz = cosf(pel_rad);
        float g_fwd  = GetGroundHeight(current_cam.x + p_fx * 0.24f, current_cam.z + p_fz * 0.24f, current_cam.y, 0.55f);
        float g_back = GetGroundHeight(current_cam.x - p_fx * 0.24f, current_cam.z - p_fz * 0.24f, current_cam.y, 0.55f);
        if (g_fwd > -500.0f && g_back > -500.0f) {
          float dy_stance = g_fwd - g_back;
          if (fabsf(dy_stance) > 0.045f && fabsf(dy_stance) < 0.70f) {
            target_stair_grade = Clamp(dy_stance / 0.18f, -1.0f, 1.0f);
          }
        }
      }
    }
    stair_grade = Lerp(stair_grade, target_stair_grade, fminf(1.0f, 14.0f * dt));

    float stair_speed_mult = 1.0f;
    if (stair_grade > 0.08f) {
      stair_speed_mult = Lerp(1.0f, 0.70f, stair_grade);
    } else if (stair_grade < -0.08f) {
      stair_speed_mult = Lerp(1.0f, 0.82f, -stair_grade);
    }

    if (wish_len > 0.01f) {
      float target_vx = wish_x * base_speed * stair_speed_mult;
      float target_vz = wish_z * base_speed * stair_speed_mult;
      float accel_rate = 26.0f;
      player_vx = Lerp(player_vx, target_vx, fminf(1.0f, accel_rate * dt));
      player_vz = Lerp(player_vz, target_vz, fminf(1.0f, accel_rate * dt));
      if (k_shift) was_sprint_active = true;
      else if (!k_shift && was_sprint_active) was_sprint_active = false;
    } else {
      float decel_rate = was_sprint_active ? 11.0f : 24.0f;
      player_vx = Lerp(player_vx, 0.0f, fminf(1.0f, decel_rate * dt));
      player_vz = Lerp(player_vz, 0.0f, fminf(1.0f, decel_rate * dt));

      float cur_speed_sq = player_vx * player_vx + player_vz * player_vz;
      if (cur_speed_sq < 0.015f) {
        player_vx = 0.0f;
        player_vz = 0.0f;
        was_sprint_active = false;
      }
    }

    float dx = player_vx * dt;
    float dz = player_vz * dt;

    if (is_arena_mode >= 2) {
      if (dx != 0.0f || dz != 0.0f) {
        float targetX = current_cam.x + dx;
        float targetZ = current_cam.z + dz;

        float step_ground = GetGroundHeight(targetX, targetZ, current_cam.y, 0.45f);
        float effective_y = current_cam.y;
        if (current_cam.is_grounded && step_ground > current_cam.y && step_ground <= current_cam.y + 0.45f) {
          effective_y = step_ground;
        }

        ResolveWalls(&targetX, &targetZ, effective_y, 0.35f, 1.8f, 0.40f);

        current_cam.x = targetX;
        current_cam.z = targetZ;

        if (world_grid.is_initialized) {
          current_cam.x = fmaxf(world_grid.minX + 1.0f, fminf(world_grid.maxX - 1.0f, current_cam.x));
          current_cam.z = fmaxf(world_grid.minZ + 1.0f, fminf(world_grid.maxZ - 1.0f, current_cam.z));
        } else {
          current_cam.x = fmaxf(-42.0f, fminf(42.0f, current_cam.x));
          current_cam.z = fmaxf(-32.0f, fminf(32.0f, current_cam.z));
        }

        if (current_cam.is_grounded) {
          float new_ground = GetGroundHeight(current_cam.x, current_cam.z, current_cam.y, 0.45f);
          if (new_ground >= current_cam.y - 0.40f && new_ground <= current_cam.y + 0.45f) {
            current_cam.y = new_ground;
          } else if (new_ground < current_cam.y - 0.40f) {
            current_cam.is_grounded = 0;
            current_cam.vel_y = 0.0f;
          }
        }
      }
    } else {
      float hitbox = 0.2f;

      // x collision
      if (dx != 0.0f) {
        float targetX = current_cam.x + dx;
        int checkX = (int)roundf((targetX + (dx > 0 ? hitbox : -hitbox)) / 2.0f);
        int pZ = (int)roundf(current_cam.z / 2.0f);
        int pZ1 = (int)roundf((current_cam.z - hitbox) / 2.0f);
        int pZ2 = (int)roundf((current_cam.z + hitbox) / 2.0f);
        int layer = (int)floorf(current_cam.y / 2.0f);

        if (GetBlock(checkX, layer, pZ) == 0 &&
            GetBlock(checkX, layer, pZ1) == 0 &&
            GetBlock(checkX, layer, pZ2) == 0) {
          current_cam.x = targetX;
        }
      }

      // z collision
      if (dz != 0.0f) {
        float targetZ = current_cam.z + dz;
        int checkZ = (int)roundf((targetZ + (dz > 0 ? hitbox : -hitbox)) / 2.0f);
        int pX = (int)roundf(current_cam.x / 2.0f);
        int pX1 = (int)roundf((current_cam.x - hitbox) / 2.0f);
        int pX2 = (int)roundf((current_cam.x + hitbox) / 2.0f);
        int layer = (int)floorf(current_cam.y / 2.0f);

        if (GetBlock(pX, layer, checkZ) == 0 &&
            GetBlock(pX1, layer, checkZ) == 0 &&
            GetBlock(pX2, layer, checkZ) == 0) {
          current_cam.z = targetZ;
        }
      }
    }

    float move_dx = current_cam.x - prev_player_x;
    float move_dz = current_cam.z - prev_player_z;
    prev_player_x = current_cam.x;
    prev_player_z = current_cam.z;
    float move_dist = sqrtf(move_dx * move_dx + move_dz * move_dz);
    bool is_moving = (move_dist > 0.0001f);

    if (!pelvis_yaw_inited) {
      pelvis_yaw = current_cam.yaw;
      pelvis_yaw_inited = true;
    }

    float strafe_hip_target = 0.0f;
    float strafe_intensity = 0.0f;
    if (k_w && !k_s) {
      if (k_a && !k_d) { strafe_hip_target = -20.0f; strafe_intensity = -0.5f; }
      else if (k_d && !k_a) { strafe_hip_target = +20.0f; strafe_intensity = +0.5f; }
    } else if (k_s && !k_w) {
      if (k_a && !k_d) { strafe_hip_target = +20.0f; strafe_intensity = -0.5f; }
      else if (k_d && !k_a) { strafe_hip_target = -20.0f; strafe_intensity = +0.5f; }
    } else if (k_a && !k_d) {
      strafe_hip_target = -32.0f;
      strafe_intensity = -1.0f;
    } else if (k_d && !k_a) {
      strafe_hip_target = +32.0f;
      strafe_intensity = +1.0f;
    }

    float yaw_diff = current_cam.yaw - pelvis_yaw;
    while (yaw_diff > 180.0f) yaw_diff -= 360.0f;
    while (yaw_diff < -180.0f) yaw_diff += 360.0f;

    static bool has_started_locomotion = false;
    static float prev_gait_sin = 0.0f;
    static float swing_settle = 1.0f;

    if (is_moving && current_cam.is_grounded) {
      has_started_locomotion = true;
      swing_settle = 1.0f;
      float move_dot = (move_dx * fx + move_dz * fz) / (move_dist > 1e-5f ? move_dist : 1.0f);
      float step_dir = (move_dot >= 0.0f) ? 1.0f : -1.0f;

      float cycle_dist = 3.0f;
      if (k_shift || was_sprint_active) cycle_dist = (is_arena_mode == 3 ? 4.2f : 3.6f);
      else if (k_ctrl) cycle_dist = 2.0f;

      gait_cycle += (move_dist / cycle_dist) * (2.0f * PI) * step_dir;
      gait_weight = Lerp(gait_weight, 1.0f, 10.0f * dt);

      float target_pelvis = current_cam.yaw + strafe_hip_target;
      float p_diff = target_pelvis - pelvis_yaw;
      while (p_diff > 180.0f) p_diff -= 360.0f;
      while (p_diff < -180.0f) p_diff += 360.0f;
      pelvis_yaw += p_diff * fminf(1.0f, 10.0f * dt);

      yaw_diff = current_cam.yaw - pelvis_yaw;
      while (yaw_diff > 180.0f) yaw_diff -= 360.0f;
      while (yaw_diff < -180.0f) yaw_diff += 360.0f;

      float cur_gait_sin = sinf(gait_cycle);
      if ((prev_gait_sin * cur_gait_sin <= 0.0f) && fabsf(cur_gait_sin - prev_gait_sin) < 1.2f) {
        float shock_mag = (k_shift || was_sprint_active) ? 0.12f : (k_ctrl ? 0.04f : 0.075f);
        if (stair_grade < -0.10f) {
          shock_mag += (-stair_grade) * 0.14f;
        }
        heel_shock_spring.vel -= shock_mag;
      }
      prev_gait_sin = cur_gait_sin;
    } else if (!current_cam.is_grounded) {
      gait_weight = Lerp(gait_weight, 0.25f, 6.0f * dt);
      swing_settle = 0.5f;
    } else {
      const float deadzone = 26.0f;
      const float max_yaw_diff = 36.0f;
      if (fabsf(yaw_diff) > max_yaw_diff) {
        pelvis_yaw = current_cam.yaw - (yaw_diff > 0.0f ? max_yaw_diff : -max_yaw_diff);
        yaw_diff = current_cam.yaw - pelvis_yaw;
        while (yaw_diff > 180.0f) yaw_diff -= 360.0f;
        while (yaw_diff < -180.0f) yaw_diff += 360.0f;
      }
      if (fabsf(yaw_diff) > deadzone) {
        float excess = fabsf(yaw_diff) - deadzone;
        float catch_speed = 36.0f + excess * 5.0f;
        float step_turn = (yaw_diff > 0.0f ? 1.0f : -1.0f) * fminf(excess, catch_speed * dt);
        pelvis_yaw += step_turn;

        gait_cycle += fabsf(step_turn) * (2.0f * PI / 60.0f);
        gait_weight = Lerp(gait_weight, 0.50f, 12.0f * dt);
        swing_settle = 1.0f;

        yaw_diff = current_cam.yaw - pelvis_yaw;
        while (yaw_diff > 180.0f) yaw_diff -= 360.0f;
        while (yaw_diff < -180.0f) yaw_diff += 360.0f;
      } else {
        swing_settle = Lerp(swing_settle, 0.0f, 14.0f * dt);
        gait_weight = Lerp(gait_weight, has_started_locomotion ? 0.70f : 0.0f, 6.0f * dt);
      }
    }

    float body_yaw_rad = (90.0f - pelvis_yaw) * DEG2RAD;
    float pitch_up_t = fmaxf(0.0f, current_cam.pitch / 89.0f);
    float pitch_dn_t = fmaxf(0.0f, -current_cam.pitch / 89.0f);
    float crouch_hip_offset = crouch_lerp * 0.048f;
    float dynamic_back_off = 0.095f + 0.035f * pitch_up_t + crouch_hip_offset;
    float body_y = (is_arena_mode >= 2) ? (smoothed_cam_y - crouch_drop) : (target_height - 1.435f);
    float stair_heave = (stair_grade > 0.10f) ? (stair_grade * 0.012f * cosf(gait_cycle * 2.0f) * gait_weight) : 0.0f;
    float pelvis_bob = -fabsf(sinf(gait_cycle)) * 0.016f * gait_weight + stair_heave;
    body_y += pelvis_bob;

    float stair_lat_shift = cosf(gait_cycle) * 0.022f * fabsf(stair_grade) * gait_weight;
    Vector3 body_pos = {
        current_cam.x - fx * dynamic_back_off + rx * stair_lat_shift,
        body_y,
        current_cam.z - fz * dynamic_back_off + rz * stair_lat_shift
    };

    static float prev_vx = 0.0f;
    static float prev_vz = 0.0f;
    static SpringDamper1D torso_lean_pitch = {0};
    static SpringDamper1D torso_lean_roll  = {0};

    float ax = (player_vx - prev_vx) / fmaxf(dt, 0.001f);
    float az = (player_vz - prev_vz) / fmaxf(dt, 0.001f);
    prev_vx = player_vx;
    prev_vz = player_vz;

    float a_fwd = ax * fx + az * fz;
    float a_right = ax * rx + az * rz;

    float fwd_run_speed = (a_fwd > 0.1f || k_w) ? ((k_shift || was_sprint_active) ? 6.2f : 3.8f) : 0.0f;
    float centripetal_bank = -Clamp((fwd_run_speed / 6.2f) * (mdx * 0.55f), -6.5f, 6.5f);

    float crouch_torso_pitch = crouch_lerp * 3.8f;

    float stair_torso_pitch = (stair_grade > 0.0f) ? (stair_grade * 4.5f) : (stair_grade * 2.0f);
    torso_lean_pitch.target = Clamp(a_fwd * 0.18f + stair_torso_pitch + crouch_torso_pitch, -6.0f, 8.0f);
    torso_lean_roll.target  = Clamp(-a_right * 0.22f + centripetal_bank, -7.5f, 7.5f);

    UpdateSpringDamper(&torso_lean_pitch, 180.0f, 22.0f, dt);
    UpdateSpringDamper(&torso_lean_roll,  180.0f, 22.0f, dt);

    static float breath_time = 0.0f;
    static float exertion = 0.0f;
    if (k_shift && (k_w || k_s || k_a || k_d)) {
      exertion = Clamp(exertion + dt * 0.35f, 0.0f, 1.0f);
    } else {
      exertion = Clamp(exertion - dt * 0.20f, 0.0f, 1.0f);
    }

    float target_hr = 72.0f;
    if (k_shift && (k_w || k_s || k_a || k_d)) {
      target_hr = 138.0f;
    } else if (k_w || k_s || k_a || k_d) {
      target_hr = 95.0f;
    }
    float hr_rate = (target_hr > heart_rate_bpm) ? 14.0f : 5.0f;
    heart_rate_bpm = Lerp(heart_rate_bpm, target_hr, fminf(1.0f, hr_rate * dt * 0.1f));

    float cardio_freq = heart_rate_bpm / 60.0f;
    cardio_phase = fmodf(cardio_phase + cardio_freq * dt, 1.0f);

    float sys_diff = cardio_phase - 0.10f;
    float systolic = expf(-(sys_diff * sys_diff) / (0.045f * 0.045f));
    float dic_diff = cardio_phase - 0.32f;
    float dicrotic = expf(-(dic_diff * dic_diff) / (0.055f * 0.055f)) * 0.32f;
    float pulse_wave = systolic + dicrotic;

    float pulse_norm = Clamp((heart_rate_bpm - 72.0f) / 72.0f, 0.0f, 1.4f);
    float pulse_scale = (0.024f + 0.020f * pulse_norm);
    float pulse_pitch = pulse_wave * pulse_scale;
    float pulse_roll  = -pulse_wave * pulse_scale * 0.65f;
    float pulse_y     = pulse_wave * 0.00018f * (1.0f + pulse_norm);

    bool is_steady_aiming = (is_aiming && ads_lerp > 0.82f && !(k_w || k_s || k_a || k_d) && wall_lift_spring.pos < 0.15f);
    if (is_steady_aiming) {
      ads_steady_timer += dt;
    } else {
      ads_steady_timer = fmaxf(0.0f, ads_steady_timer - 3.5f * dt);
    }

    float target_pause = 1.0f;
    if (ads_steady_timer > 0.7f) {
      float p_t = Clamp((ads_steady_timer - 0.7f) / 0.8f, 0.0f, 1.0f);
      p_t = p_t * p_t * (3.0f - 2.0f * p_t);
      target_pause = Lerp(1.0f, 0.12f, p_t);
    }
    breath_pause_factor = Lerp(breath_pause_factor, target_pause, fminf(1.0f, 6.0f * dt));

    if (ads_steady_timer > 6.5f) {
      float hyp_t = Clamp((ads_steady_timer - 6.5f) / 3.5f, 0.0f, 1.0f);
      tremor_time += dt * 56.0f;
      float tremor_mag = hyp_t * (0.028f + 0.020f * exertion);
      hypoxia_tremor_pitch = sinf(tremor_time) * tremor_mag;
      hypoxia_tremor_yaw   = cosf(tremor_time * 0.83f) * tremor_mag * 0.65f;
    } else {
      hypoxia_tremor_pitch = 0.0f;
      hypoxia_tremor_yaw   = 0.0f;
    }

    breath_time += dt * (1.4f + 2.0f * exertion);
    float breath_amp = (1.0f + 1.8f * exertion) * breath_pause_factor;
    float ribcage_breath = sinf(breath_time) * 0.0020f * breath_amp;

    float walk_hip_twist = sinf(gait_cycle) * (3.5f * DEG2RAD) * gait_weight;
    SayoriSpineState spine_state = { 0 };
    if (has_sayori_model && (is_arena_mode >= 2 || cur_wp == 1) && s_upper_body >= 0 && s_upper_body2 >= 0) {
      ComputeSayoriSpinePose(&spine_state, &sayoriModel, s_upper_body, s_upper_body2, s_neck, s_head,
                             &s_chain_r, &s_chain_l, current_cam.pitch, yaw_diff, walk_hip_twist,
                             torso_lean_pitch.pos, torso_lean_roll.pos, ribcage_breath, body_yaw_rad, body_pos);
      camera.position.x = current_cam.x;
      camera.position.y = target_height + pelvis_bob;
      camera.position.z = current_cam.z;
    } else {
      camera.position.x = current_cam.x;
      camera.position.y = target_height;
      camera.position.z = current_cam.z;
    }

    float vis_pitch_rad = (current_cam.pitch + cam_recoil_pitch_spring.pos) * DEG2RAD;
    Vector3 vis_cam_fwd = {
        cosf(vis_pitch_rad) * cosf(phys_yawRad),
        sinf(vis_pitch_rad),
        cosf(vis_pitch_rad) * sinf(phys_yawRad)
    };
    camera.target = Vector3Add(camera.position, vis_cam_fwd);

    Vector3 flat_fwd = { fx, 0.0f, fz };
    Vector3 flat_right = { rx, 0.0f, rz };
    float wall_target = 0.0f;
    if (cur_wp == 1 && has_sayori_model) {
      Vector3 chest_origin = { current_cam.x, camera.position.y - 0.13f, current_cam.z };
      Vector3 eye_origin   = { current_cam.x, camera.position.y - 0.02f, current_cam.z };
      Vector3 left_origin  = Vector3Add(chest_origin, Vector3Scale(flat_right, -0.07f));
      Vector3 right_origin = Vector3Add(chest_origin, Vector3Scale(flat_right, +0.07f));

      float d_c = GetForwardWallDistance(chest_origin, flat_fwd, 0.85f, current_cam.y);
      float d_e = GetForwardWallDistance(eye_origin,   flat_fwd, 0.85f, current_cam.y);
      float d_l = GetForwardWallDistance(left_origin,  flat_fwd, 0.85f, current_cam.y);
      float d_r = GetForwardWallDistance(right_origin, flat_fwd, 0.85f, current_cam.y);
      float wall_dist = fminf(fminf(d_c, d_e), fminf(d_l, d_r));

      float u_wall = Clamp((0.72f - wall_dist) / (0.72f - 0.36f), 0.0f, 1.0f);
      wall_target = u_wall * u_wall * (3.0f - 2.0f * u_wall);

      float pitch_up_clear = Clamp((current_cam.pitch - 10.0f) / 45.0f, 0.0f, 1.0f);
      wall_target *= (1.0f - 0.70f * pitch_up_clear);
    }
    wall_lift_spring.target = wall_target;
    UpdateSpringDamper(&wall_lift_spring, 190.0f, 24.0f, dt);
    float wall_lift_t = Clamp(wall_lift_spring.pos, 0.0f, 1.08f);

    float ads_eff = ads_lerp * (1.0f - Clamp(wall_lift_t, 0.0f, 1.0f));
    float ads_rot_eff = Clamp(ads_rot_spring.pos, -0.15f, 1.15f) * (1.0f - Clamp(wall_lift_t, 0.0f, 1.0f));

    bool wants_sprint_lower = (cur_wp == 1 && (k_shift || was_sprint_active) && (k_w || k_s || k_a || k_d) && !is_aiming && wall_lift_t < 0.25f);
    float sul_rate = (is_aiming || k_shoot) ? 26.0f : (wants_sprint_lower ? 8.5f : 14.0f);
    sprint_sul_t = Lerp(sprint_sul_t, wants_sprint_lower ? 1.0f : 0.0f, fminf(1.0f, sul_rate * dt));

    float cam_roll_deg = (cur_wp == 1 ? (0.9f * ads_eff) : 0.0f) + lag_roll.pos * 0.08f + torso_lean_roll.pos * 0.12f;
    float cam_roll_rad = cam_roll_deg * DEG2RAD;
    Vector3 cam_fwd_n = Vector3Normalize(vis_cam_fwd);
    Vector3 cam_right_n, cam_up_n;
    BuildRollBasis(cam_fwd_n, flat_right, cam_roll_rad, &cam_right_n, &cam_up_n);
    camera.up = cam_up_n;
    camera.fovy = Lerp(60.0f, (cur_wp == 1 ? 42.0f : 40.0f), ads_eff);

    float breath_x = sinf(breath_time) * 0.00065f * breath_amp * (1.0f - ads_eff * 0.65f);
    float breath_y = sinf(breath_time * 2.0f) * 0.00045f * breath_amp * (1.0f - ads_eff * 0.65f);

    static float bob_time = 0.0f;
    float current_speed = (k_w || k_s || k_a || k_d) ? 1.0f : 0.0f;
    if (k_shift) current_speed *= 1.6f;
    else if (k_ctrl) current_speed *= 0.5f;

    static float bob_amp = 0.0f;
    bob_amp = Lerp(bob_amp, current_speed, 10.0f * dt);
    if (bob_amp > 0.01f) {
      bob_time += dt * 11.0f;
    } else {
      bob_time = Lerp(bob_time, 0.0f, 8.0f * dt);
    }

    float bob_x = sinf(bob_time * 0.5f) * 0.0040f * bob_amp * (1.0f - ads_eff * 0.72f);
    float bob_y = cosf(bob_time) * 0.0030f * bob_amp * (1.0f - ads_eff * 0.72f) + heel_shock_spring.pos * (1.0f - ads_eff * 0.55f);

    WeaponTransform def = default_weapon_transforms[cur_wp];
    float base_hip_x = def.hip_pos.x + current_input.wp_hip_x;
    float base_hip_y = def.hip_pos.y + current_input.wp_hip_y;
    float base_hip_z = def.hip_pos.z + current_input.wp_hip_z;
    float base_ads_x = def.ads_pos.x + current_input.wp_ads_x;
    float base_ads_y = def.ads_pos.y + current_input.wp_ads_y;
    float base_ads_z = def.ads_pos.z + current_input.wp_ads_z;
    float base_pitch = def.pitch + current_input.wp_pitch;
    float base_yaw   = def.yaw + current_input.wp_yaw;
    float base_roll  = def.roll + current_input.wp_roll;
    float wp_scale   = def.scale + current_input.wp_scale;
    if (wp_scale < 0.001f) wp_scale = def.scale;

    Matrix gunWorldMat = MatrixIdentity();
    Matrix gunAnchor   = MatrixIdentity();
    if (cur_wp == 1 && has_sayori_model) {
      const float ADS_GUN_CANT_DEG = 3.0f;
      float stair_pitch_bias = stair_grade * 6.5f * (1.0f - ads_eff * 0.45f);
      float gun_track_pitch = current_cam.pitch + stair_pitch_bias;
      float min_gun_pitch = Lerp(-64.0f, -84.0f, ads_eff);
      float max_gun_pitch = Lerp(+80.0f, +86.0f, ads_eff);
      float look_down_excess = 0.0f;
      if (gun_track_pitch < min_gun_pitch) {
        look_down_excess = (min_gun_pitch - gun_track_pitch) / 32.0f;
        gun_track_pitch = min_gun_pitch;
      } else if (gun_track_pitch > max_gun_pitch) {
        gun_track_pitch = max_gun_pitch;
      }
      float g_pitch_rad = gun_track_pitch * DEG2RAD;
      Vector3 g_fwd_base = { cosf(g_pitch_rad) * fx, sinf(g_pitch_rad), cosf(g_pitch_rad) * fz };
      Vector3 g_right_base, g_up_base;
      BuildRollBasis(g_fwd_base, flat_right, cam_roll_rad, &g_right_base, &g_up_base);

      float ld = Clamp((-current_cam.pitch - 10.0f) / 55.0f, 0.0f, 1.0f);
      ld = ld * ld * (3.0f - 2.0f * ld);
      float sternum_arc = sinf(Clamp(-current_cam.pitch / 62.0f, 0.0f, 1.0f) * PI * 0.5f) * 0.065f;

      float dy_neck = -0.055f * pitch_up_t;
      float dz_neck = -0.060f * pitch_up_t;

      Vector3 anchor_origin = camera.position;
      anchor_origin.y += dy_neck - (1.0f - ads_eff) * (look_down_excess * 0.08f);
      anchor_origin = Vector3Add(anchor_origin, Vector3Scale(flat_fwd, (1.0f - ads_eff) * (ld * 0.045f + sternum_arc) + dz_neck));
      gunAnchor = MatrixFromBasisOrigin(g_right_base, g_up_base, Vector3Negate(g_fwd_base), anchor_origin);

      float pitch_up_t = Clamp((current_cam.pitch - 15.0f) / 65.0f, 0.0f, 1.0f);
      pitch_up_t = pitch_up_t * pitch_up_t * (3.0f - 2.0f * pitch_up_t);
      float reach_up_ready = pitch_up_t * 0.025f;
      float reach_up_ads   = pitch_up_t * 0.035f;

      float pitch_dn_t = Clamp((-current_cam.pitch - 10.0f) / 60.0f, 0.0f, 1.0f);
      pitch_dn_t = pitch_dn_t * pitch_dn_t * (3.0f - 2.0f * pitch_dn_t);
      float reach_dn_ready = pitch_dn_t * 0.045f;
      float reach_dn_ads   = pitch_dn_t * 0.048f;

      float center_t = fmaxf(ld, pitch_up_t * 0.85f);
      float ready_x_scale = 1.0f - 0.75f * center_t;
      float ready_y_lift  = pitch_up_t * 0.015f;

      Vector3 pos_ready = {
          base_hip_x * ready_x_scale + bob_x + breath_x + lag_trans_x.pos,
          base_hip_y + ready_y_lift + bob_y + breath_y + lag_trans_y.pos + pulse_y * 0.35f,
          base_hip_z - reach_up_ready - reach_dn_ready - ld * 0.03f
      };
      float ads_cant_shift = 0.061f * sinf(ADS_GUN_CANT_DEG * DEG2RAD) * ads_rot_eff;
      Vector3 pos_ads = {
          base_ads_x + ads_cant_shift + (bob_x + breath_x) * 0.18f + lag_trans_x.pos * 0.20f,
          base_ads_y + (bob_y + breath_y) * 0.18f + lag_trans_y.pos * 0.20f + pulse_y,
          base_ads_z - reach_up_ads - reach_dn_ads
      };
      Vector3 pos_wall = {
          +0.062f + bob_x * 0.4f,
          -0.075f + bob_y * 0.4f,
          -0.195f
      };
      Vector3 pos_sprint_lower = {
          base_hip_x + 0.018f + bob_x * 0.45f,
          -0.245f + bob_y * 0.60f + sinf(gait_cycle * 2.0f) * 0.010f,
          base_hip_z + 0.035f + cosf(gait_cycle) * 0.018f
      };

      Vector3 pos_aim_blend = Vector3Lerp(pos_ready, pos_ads, ads_eff);
      Vector3 pos_sprint_blend = Vector3Lerp(pos_aim_blend, pos_sprint_lower, sprint_sul_t);
      Vector3 gun_rel_pos = Vector3Lerp(pos_sprint_blend, pos_wall, Clamp(wall_lift_t, 0.0f, 1.0f));
      gun_rel_pos.z += recoil_z_spring.pos + slide_blowback * 0.012f + inertia_elbow_flex.pos;

      float sul_pitch_add = -16.0f * sprint_sul_t;
      float sul_yaw_add   = +3.5f  * sprint_sul_t;
      float sul_roll_add  = -2.0f  * sprint_sul_t;

      float wall_pitch_add = Clamp(68.0f - current_cam.pitch, 0.0f, 68.0f);
      float stance_pitch = Lerp(Lerp(-1.0f, 0.0f, ads_rot_eff), wall_pitch_add, wall_lift_t) + sul_pitch_add;
      float stance_yaw   = Lerp(Lerp(+2.0f, 0.0f, ads_rot_eff), +12.0f, wall_lift_t) + sul_yaw_add;
      float stance_roll  = Lerp(Lerp(+11.0f, ADS_GUN_CANT_DEG, ads_rot_eff), +22.0f, wall_lift_t) + sul_roll_add;

      float tot_pitch = stance_pitch + base_pitch + recoil_pitch_spring.pos + lag_pitch.pos + heel_shock_spring.pos * 18.0f + pulse_pitch + hypoxia_tremor_pitch;
      float tot_yaw   = stance_yaw   + base_yaw   + lag_yaw.pos + recoil_yaw_spring.pos + hypoxia_tremor_yaw;
      float tot_roll  = stance_roll  + base_roll  + lag_roll.pos + recoil_roll_spring.pos + pulse_roll;

      Matrix m_loc_rot = MatrixMultiply(
          MatrixMultiply(MatrixRotateZ(tot_roll * DEG2RAD), MatrixRotateX(tot_pitch * DEG2RAD)),
          MatrixRotateY(tot_yaw * DEG2RAD)
      );
      Matrix m_loc = MatrixMultiply(m_loc_rot, MatrixTranslate(gun_rel_pos.x, gun_rel_pos.y, gun_rel_pos.z));
      gunWorldMat = MatrixMultiply(m_loc, gunAnchor);

      {
        TetherGunToShoulders(&gunWorldMat, spine_state.sh_world_r, spine_state.sh_world_l, &sayoriModel, &s_chain_l, &s_chain_r, ads_eff, sprint_sul_t);
      }
    }

    bool can_shoot_wp = false;
    if (cur_wp == 1) {
      if (k_shoot && shoot_cooldown <= 0.0f && trigger_has_reset) {
        can_shoot_wp = true;
      }
    } else {
      if (k_shoot && shoot_cooldown <= 0.0f) {
        can_shoot_wp = true;
      }
    }

    if (can_shoot_wp) {
      WeaponDef w = weapons[cur_wp];
      shoot_cooldown = w.cooldown;

      float base_dmg = w.damage;
      if (cur_wp == 1) {
        base_dmg *= (1.0f + current_input.pistol_lvl * 0.01f);
      } else if (cur_wp == 2) {
        base_dmg *= (1.0f + current_input.shotgun_lvl * 0.01f);
      } else if (cur_wp == 3) {
        base_dmg *= (1.0f + current_input.minigun_lvl * 0.10f);
      }

      float spread_mult = is_aiming ? 0.5f : 1.0f;
      Vector3 ray_origin = camera.position;
      Vector3 base_ray_dir = forward;
      if (cur_wp == 1 && has_sayori_model) {
        ray_origin = Vector3Transform((Vector3){ 0.0f, 0.0455f, -0.1721f }, gunWorldMat);
        base_ray_dir = Vector3Normalize(TransformDirByMatrix((Vector3){ 0.0f, 0.0f, -1.0f }, gunWorldMat));
      }

      for (int p = 0; p < w.pellets; p++) {
        int hit_idx = -1;
        float closest_dist = 25.0f;

        float spread_x = ((float)GetRandomValue(-100, 100) / 100.0f) * w.spread * spread_mult;
        float spread_y = ((float)GetRandomValue(-100, 100) / 100.0f) * w.spread * spread_mult;

        Vector3 ray_dir = base_ray_dir;
        ray_dir.x += spread_x;
        ray_dir.y += spread_y;
        ray_dir.z += spread_x;
        ray_dir = Vector3Normalize(ray_dir);

        for (int i = 0; i < num_enemies; i++) {
          if (enemies[i].state >= 3)
            continue;

          float ex = enemies[i].x;
          float ey = enemies[i].y;
          float ez = enemies[i].z;

          float vx = ex - ray_origin.x;
          float vy = ey - ray_origin.y;
          float vz = ez - ray_origin.z;

          float t_closest = vx * ray_dir.x + vy * ray_dir.y + vz * ray_dir.z;

          if (t_closest > 0.0f && t_closest < closest_dist) {
            float proj_x = ray_origin.x + ray_dir.x * t_closest;
            float proj_z = ray_origin.z + ray_dir.z * t_closest;

            float dist_sq =
                (ex - proj_x) * (ex - proj_x) + (ez - proj_z) * (ez - proj_z);
            if (dist_sq < (0.35f * 0.35f)) {
              float hit_y = ray_origin.y + ray_dir.y * t_closest;
              if (hit_y >= (ey - 0.15f) && hit_y <= (ey + 1.65f)) {
                closest_dist = t_closest;
                hit_idx = i;
              }
            }
          }
        }

        if (hit_idx == -1) {
          for (float st = 0.5f; st < closest_dist; st += 0.25f) {
            Vector3 test_p = Vector3Add(ray_origin, Vector3Scale(ray_dir, st));
            int bx = (int)roundf(test_p.x / 2.0f);
            int bz = (int)roundf(test_p.z / 2.0f);
            int by = (int)floorf(test_p.y / 2.0f);
            if (GetBlock(bx, by, bz) != 0) {
              SpawnBulletSparks(test_p, Vector3Scale(ray_dir, -1.0f), (Color){ 255, 205, 75, 255 });
              break;
            }
          }
        }
        if (hit_idx != -1) {
          if (enemies[hit_idx].type == 3 && enemies[hit_idx].dodge_timer <= 0.0f) {
            float dodge_dx = -forward.z * 1.5f;
            float dodge_dz = forward.x * 1.5f;
            int check_x = (int)roundf((enemies[hit_idx].x + dodge_dx) / 2.0f);
            int check_z = (int)roundf((enemies[hit_idx].z + dodge_dz) / 2.0f);
            int el = (int)floorf(enemies[hit_idx].y / 2.0f);
            if (GetBlock(check_x, el, check_z) == 0) {
              enemies[hit_idx].x += dodge_dx;
              enemies[hit_idx].z += dodge_dz;
            }
            enemies[hit_idx].dodge_timer = 4.0f;
          } else {
            enemies[hit_idx].hp -= base_dmg;
            SendEvent(10, (int)base_dmg);
            Vector3 hit_p = Vector3Add(ray_origin, Vector3Scale(ray_dir, closest_dist));
            SpawnBulletSparks(hit_p, Vector3Scale(ray_dir, -1.0f), (Color){ 255, 60, 60, 255 });
            if (enemies[hit_idx].hp <= 0.0f) {
              enemies[hit_idx].state = 3;
              SendEvent(11, enemies[hit_idx].type);

              if (num_static_sprites < MAX_SPRITES) {
                int drop = -1;
                int r = rand() % 100;
                if (enemies[hit_idx].type == 1 || enemies[hit_idx].type == 9) {
                  drop = 12;
                } else if (r < 35) {
                  drop = 11;
                } else if (r < 45) {
                  drop = 7;
                } else if (r < 55) {
                  drop = 13;
                } else if (r < 65) {
                  drop = 15;
                }

                if (drop != -1) {
                  static_sprites[num_static_sprites].x = enemies[hit_idx].x;
                  static_sprites[num_static_sprites].y = enemies[hit_idx].y;
                  static_sprites[num_static_sprites].z = enemies[hit_idx].z;
                  static_sprites[num_static_sprites].type = drop;
                  static_sprites[num_static_sprites].state = 0;
                  num_static_sprites++;
                }
              }
            }
          }
        }
      }

      if (cur_wp > 0) {
        muzzle_flash_timer = 0.08f;
      }

      SendEvent(20, cur_wp); // shoot weapon event
      if (cur_wp == 1) {
        slide_blowback = 1.0f;
        trigger_pull = 1.0f;
        trigger_travel = 1.0f;
        trigger_sear_broken = true;
        trigger_has_reset = false;

        recoil_pitch_spring.vel += is_aiming ? 135.0f : 175.0f;
        recoil_z_spring.vel     += is_aiming ? 0.42f : 0.58f;
        recoil_roll_spring.vel  += is_aiming ? -38.0f : -56.0f;
        recoil_yaw_spring.vel   += is_aiming ? -12.0f : -20.0f;
        cam_recoil_pitch_spring.vel += is_aiming ? 16.0f : 26.0f;

        support_shear_z.vel   += is_aiming ? 0.055f : 0.090f;
        support_shear_yaw.vel += is_aiming ? 22.0f : 36.0f;

        heart_rate_bpm = fminf(165.0f, heart_rate_bpm + 3.5f);
        ads_steady_timer = 0.0f;
      } else {
        recoil_offset_z = w.recoil_z;
        recoil_pitch = w.recoil_pitch;
      }
    }

    SetShaderValue(lighting_shader, tod_loc, &current_cam.time_of_day,
                   SHADER_UNIFORM_FLOAT);
    SetShaderValue(lighting_shader, lq_loc, &current_cam.lighting_quality,
                   SHADER_UNIFORM_INT);
    SetShaderValue(lighting_shader, sh_loc, &current_cam.shadows_enabled,
                   SHADER_UNIFORM_INT);

    BeginTextureMode(target);
    ClearBackground(BLANK);
    rlSetClipPlanes(0.02, 4000.0);
    BeginMode3D(camera);

    float t_cel = fmodf(current_cam.time_of_day, 24.0f);
    float sunAngle_cel = ((t_cel - 6.0f) / 24.0f) * 6.28318530718f;
    float sunElev_cel = sinf(sunAngle_cel);
    Vector3 world_light_dir = {cosf(sunAngle_cel), sunElev_cel > 0.15f ? sunElev_cel : 0.15f, 0.25f * cosf(sunAngle_cel)};

    Vector3 world_light_col;
    Vector3 world_amb_col;
    if (sunElev_cel > 0.15f) {
      world_light_col = (Vector3){1.10f, 1.05f, 0.95f};
      world_amb_col   = (Vector3){0.55f, 0.58f, 0.65f};
    } else if (sunElev_cel > -0.15f) {
      float sunset_t = (sunElev_cel + 0.15f) / 0.30f;
      world_light_col = Vector3Lerp((Vector3){0.40f, 0.45f, 0.65f}, (Vector3){1.15f, 0.85f, 0.60f}, sunset_t);
      world_amb_col   = Vector3Lerp((Vector3){0.20f, 0.22f, 0.32f}, (Vector3){0.48f, 0.45f, 0.55f}, sunset_t);
    } else {
      world_light_col = (Vector3){0.25f, 0.30f, 0.45f};
      world_amb_col   = (Vector3){0.14f, 0.16f, 0.24f};
    }

    Vector3 world_view_pos = {camera.position.x, camera.position.y, camera.position.z};
    float world_rim_pow = 0.0f;
    Vector3 world_rim_col = {0.75f, 0.85f, 1.0f};

    SetShaderValue(cel_shader, cel_light_dir_loc, &world_light_dir, SHADER_UNIFORM_VEC3);
    SetShaderValue(cel_shader, cel_light_col_loc, &world_light_col, SHADER_UNIFORM_VEC3);
    SetShaderValue(cel_shader, cel_amb_col_loc, &world_amb_col, SHADER_UNIFORM_VEC3);
    SetShaderValue(cel_shader, cel_view_pos_loc, &world_view_pos, SHADER_UNIFORM_VEC3);
    SetShaderValue(cel_shader, cel_rim_pow_loc, &world_rim_pow, SHADER_UNIFORM_FLOAT);
    SetShaderValue(cel_shader, cel_rim_col_loc, &world_rim_col, SHADER_UNIFORM_VEC3);

    SetShaderValue(dyn_light_shader, dyn_view_pos_loc, &world_view_pos, SHADER_UNIFORM_VEC3);
    SetShaderValue(dyn_light_shader, dyn_sun_dir_loc, &world_light_dir, SHADER_UNIFORM_VEC3);
    SetShaderValue(dyn_light_shader, dyn_sun_col_loc, &world_light_col, SHADER_UNIFORM_VEC3);
    SetShaderValue(dyn_light_shader, dyn_amb_col_loc, &world_amb_col, SHADER_UNIFORM_VEC3);

    int fl_active_int = current_input.flashlight_active ? 1 : 0;
    SetShaderValue(dyn_light_shader, dyn_fl_active_loc, &fl_active_int, SHADER_UNIFORM_INT);

    Vector3 fl_pos = Vector3Subtract(camera.position, Vector3Scale(vis_cam_fwd, 0.20f));
    fl_pos = Vector3Add(fl_pos, Vector3Scale(flat_right, 0.12f));
    fl_pos.y -= 0.06f;
    Vector3 fl_dir = vis_cam_fwd;
    Vector3 fl_col = (Vector3){ 1.30f, 1.26f, 1.18f };
    SetShaderValue(dyn_light_shader, dyn_fl_pos_loc, &fl_pos, SHADER_UNIFORM_VEC3);
    SetShaderValue(dyn_light_shader, dyn_fl_dir_loc, &fl_dir, SHADER_UNIFORM_VEC3);
    SetShaderValue(dyn_light_shader, dyn_fl_col_loc, &fl_col, SHADER_UNIFORM_VEC3);

    float muz_int = (muzzle_flash_timer > 0.0f) ? (muzzle_flash_timer / 0.08f) : 0.0f;
    Vector3 muz_pos = (cur_wp == 1 && has_sayori_model)
        ? Vector3Transform((Vector3){ 0.0f, 0.0455f, -0.1721f }, gunWorldMat)
        : Vector3Add(camera.position, Vector3Scale(vis_cam_fwd, 0.55f));
    Vector3 muz_col = (Vector3){ 2.6f, 1.95f, 0.90f };
    SetShaderValue(dyn_light_shader, dyn_muz_int_loc, &muz_int, SHADER_UNIFORM_FLOAT);
    SetShaderValue(dyn_light_shader, dyn_muz_pos_loc, &muz_pos, SHADER_UNIFORM_VEC3);
    SetShaderValue(dyn_light_shader, dyn_muz_col_loc, &muz_col, SHADER_UNIFORM_VEC3);

    int flat_norm_world = 1;
    SetShaderValue(dyn_light_shader, dyn_flat_norm_loc, &flat_norm_world, SHADER_UNIFORM_INT);

    if (is_arena_mode >= 2 && has_world3d_model) {
      Matrix matView = rlGetMatrixModelview();
      Matrix matProj = rlGetMatrixProjection();
      Matrix matVP = MatrixMultiply(matView, matProj);
      Frustum frustum = ExtractFrustum(matVP);

      for (int m = 0; m < world3DModel.meshCount; m++) {
        if (world3d_mesh_boxes != NULL && !FrustumContainsBox(frustum, world3d_mesh_boxes[m])) {
          continue;
        }
        DrawMesh(world3DModel.meshes[m], world3DModel.materials[world3DModel.meshMaterial[m]], world3DModel.transform);
      }
    } else if (dynamic_map != NULL) {
      for (int l = 0; l < map_layers; l++) {
        for (int z = 0; z < map_height; z++) {
          for (int x = 0; x < map_width; x++) {
            int tile =
                dynamic_map[(l * map_height * map_width) + (z * map_width) + x];
            if (tile > 0) {
              int tex_idx = tile - 1;
              if (tex_idx < 0)
                tex_idx = 0;
              if (tex_idx > 8)
                tex_idx = 8;

              cubeModel.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture =
                  wall_textures[tex_idx];
              DrawModel(cubeModel,
                        (Vector3){x * 2.0f, l * 2.0f + 1.0f, z * 2.0f}, 1.0f,
                        WHITE);
            }
          }
        }
      }

      floorModel.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = floor_tex;
      for (int z = 0; z < map_height; z++) {
        for (int x = 0; x < map_width; x++) {
          DrawModel(floorModel, (Vector3){x * 2.0f, -0.5f, z * 2.0f}, 1.0f,
                    WHITE);
        }
      }
    }

    // sprites
    for (int i = 0; i < num_static_sprites; i++) {
      if (static_sprites[i].state >= 0) {
        int type = static_sprites[i].type;
        if (type >= 0 && type < 16) {
          Vector3 pos = {static_sprites[i].x, static_sprites[i].y + 1.0f,
                         static_sprites[i].z};
          DrawBillboard(camera, sprite_tex[type], pos, 2.0f, WHITE);
        }

        if (type == 0 || type == 6 || type == 7 || type == 8 || type == 11 ||
            type == 12 || type >= 13) {
          float dist_sq = (current_cam.x - static_sprites[i].x) *
                              (current_cam.x - static_sprites[i].x) +
                          (current_cam.z - static_sprites[i].z) *
                              (current_cam.z - static_sprites[i].z);
          if (dist_sq < 0.36f) { // 0.6f * 0.6f
            static_sprites[i].state = -1;
            SendEvent(1, static_sprites[i].type); // pickup
          }
        }
      }
    }

    // projectile update and 3D billboard
    for (int p = 0; p < MAX_PROJECTILES; p++) {
      if (projectiles[p].active) {
        projectiles[p].pos = Vector3Add(projectiles[p].pos,
                                        Vector3Scale(projectiles[p].dir, projectiles[p].speed * dt));
        
        int bx = (int)roundf(projectiles[p].pos.x / 2.0f);
        int bz = (int)roundf(projectiles[p].pos.z / 2.0f);
        int layer = (int)floorf(projectiles[p].pos.y / 2.0f);
        if (GetBlock(bx, layer, bz) > 0) {
          projectiles[p].active = 0;
          continue;
        }

        if (!projectiles[p].from_player) {
          float p_dist_sq = (projectiles[p].pos.x - camera.position.x) * (projectiles[p].pos.x - camera.position.x) +
                            (projectiles[p].pos.y - camera.position.y) * (projectiles[p].pos.y - camera.position.y) +
                            (projectiles[p].pos.z - camera.position.z) * (projectiles[p].pos.z - camera.position.z);
          if (p_dist_sq < 0.4f) {
            projectiles[p].active = 0;
            Vector3 incoming = Vector3Scale(projectiles[p].dir, -1.0f);
            float world_angle_deg = atan2f(incoming.z, incoming.x) * RAD2DEG;
            int angle_encoded = (int)roundf(world_angle_deg);
            SendEvent(12, angle_encoded);
            continue;
          }
        }

        int t_idx = projectiles[p].tex_idx;
        if (t_idx >= 0 && t_idx < 16) {
          DrawBillboard(camera, sprite_tex[t_idx], projectiles[p].pos, 0.5f, WHITE);
        }
      }
    }

    int flat_norm_char = 0;
    SetShaderValue(dyn_light_shader, dyn_flat_norm_loc, &flat_norm_char, SHADER_UNIFORM_INT);

    float char_rim_pow = 0.12f;
    SetShaderValue(cel_shader, cel_rim_pow_loc, &char_rim_pow, SHADER_UNIFORM_FLOAT);
    for (int i = 0; i < num_enemies; i++) {
      if (enemies[i].state >= 0) {
        if (enemies[i].state < 3) {
          enemies[i].move_timer += GetFrameTime();
          if (enemies[i].move_timer > 1.0f / 30.0f) {
            enemies[i].anim_frame++;
            enemies[i].move_timer = 0.0f;
          }
          if (guyAnimsCount > 0) {
            int max_frames = guyAnims[0].keyframeCount;
            if (enemies[i].anim_frame >= max_frames)
              enemies[i].anim_frame = 0;
            UpdateModelAnimation(guyModel, guyAnims[0], enemies[i].anim_frame);
          }
        }

        float edx = camera.position.x - enemies[i].x;
        float edz = camera.position.z - enemies[i].z;
        float angle = atan2f(edx, edz) * RAD2DEG;

        Vector3 pos = {enemies[i].x, enemies[i].y, enemies[i].z};
        
        Color tint = WHITE;
        float model_scale = 0.5f;

        // TODO: remove debug colores
        if (enemies[i].type == 1 || enemies[i].type == 9) { // yuritler
          tint = (Color){255, 200, 50, 255};
          model_scale = 0.65f;
        } else if (enemies[i].type == 2) {
          tint = (Color){200, 40, 40, 255};
        } else if (enemies[i].type == 3) {
          tint = (Color){50, 160, 240, 255};
        }

        if (enemies[i].state >= 3) {
          tint = GRAY;
        }

        DrawModelEx(guyModel, pos, (Vector3){0, 1, 0}, angle,
                    (Vector3){model_scale, model_scale, model_scale}, tint);
      }
    }

    if (has_sayori_model && (is_arena_mode >= 2 || cur_wp == 1)) {
      Matrix bodyMat = MatrixMultiply(
          MatrixRotateY(body_yaw_rad),
          MatrixTranslate(body_pos.x, body_pos.y, body_pos.z)
      );
      Matrix invBodyMat = MatrixInvertRigid(bodyMat);

      if (sayori_anim.keyframePoses && sayori_anim.keyframePoses[0]) {
        for (int b = 0; b < (int)sayori_anim.boneCount; b++) {
          sayori_anim.keyframePoses[0][b] = sayoriModel.skeleton.bindPose[b];
          if (b < 256 && (spine_state.accum_rot[b].w != 1.0f || spine_state.accum_rot[b].x != 0.0f ||
                          spine_state.accum_rot[b].y != 0.0f || spine_state.accum_rot[b].z != 0.0f ||
                          b == s_upper_body || b == s_upper_body2)) {
            sayori_anim.keyframePoses[0][b].translation = spine_state.posed_pos[b];
            sayori_anim.keyframePoses[0][b].rotation = QuaternionMultiply(spine_state.accum_rot[b], sayoriModel.skeleton.bindPose[b].rotation);
          }
        }

        float hip_twist_y = sinf(gait_cycle) * (3.5f * DEG2RAD) * gait_weight;
        float stair_pelvic_roll = cosf(gait_cycle) * (3.8f * DEG2RAD) * fabsf(stair_grade) * gait_weight;
        float pelvic_tilt_z = cosf(gait_cycle) * (2.0f * DEG2RAD) * gait_weight + stair_pelvic_roll;
        if (s_lower_body >= 0) {
          Quaternion q_pelvis = QuaternionMultiply(
            QuaternionFromAxisAngle((Vector3){0, 1, 0}, hip_twist_y),
            QuaternionFromAxisAngle((Vector3){0, 0, 1}, pelvic_tilt_z)
          );
          sayori_anim.keyframePoses[0][s_lower_body].rotation = QuaternionMultiply(
            sayoriModel.skeleton.bindPose[s_lower_body].rotation, q_pelvis
          );
        }

        float L1 = 0.4106f;
        float L2 = 0.3904f;
        float hip_y = 0.8955f;
        float ankle_y = 0.0980f;

        float actual_speed = sqrtf(player_vx * player_vx + player_vz * player_vz);
        float sprint_t = 0.0f;
        if (actual_speed > 3.8f) {
          sprint_t = fminf(1.0f, (actual_speed - 3.8f) / 2.47f);
        }

        float base_reach = Lerp(0.30f, 0.40f, sprint_t);
        float base_push = Lerp(0.24f, 0.32f, sprint_t);
        float base_lift = Lerp(0.08f, 0.13f, sprint_t);
        if (wants_crouch) {
          base_reach = 0.20f;
          base_push = 0.16f;
          base_lift = 0.045f;
        }

        if (stair_grade > 0.10f) {
          base_lift = Lerp(base_lift, 0.185f, stair_grade);
        }

        float stride_reach = base_reach * gait_weight;
        float stride_push = base_push * gait_weight;
        float lift_max = base_lift * gait_weight * swing_settle;

        float base_strafe_reach = 0.14f * strafe_intensity;
        float base_strafe_push  = 0.10f * strafe_intensity;
        float strafe_reach = base_strafe_reach * gait_weight;
        float strafe_push  = base_strafe_push  * gait_weight;

        static float sm_ground_l = 0.0f;
        static float sm_ground_r = 0.0f;

        static int stair_lead_foot = 0; // 0: left, 1: right
        if (is_moving) {
          float g_sin = sinf(gait_cycle);
          if (g_sin > 0.25f) stair_lead_foot = 0;
          else if (g_sin < -0.25f) stair_lead_foot = 1;
        }

        Vector3 pos_hip_l = {0}, pos_knee_l = {0};
        Vector3 pos_hip_r = {0}, pos_knee_r = {0};

        for (int leg = 0; leg < 2; leg++) {
          int hip_b = (leg == 0) ? s_hip_l : s_hip_r;
          int knee_b = (leg == 0) ? s_knee_l : s_knee_r;
          int ankle_b = (leg == 0) ? s_ankle_l : s_ankle_r;
          int toe_b = (leg == 0) ? s_toe_l : s_toe_r;
          if (hip_b < 0 || knee_b < 0 || ankle_b < 0) continue;

          float phi = (leg == 0) ? gait_cycle : (gait_cycle + PI);

          float dz_raw, dx_raw, lift_raw, ankle_tilt_deg;
          GetBiomechanicalFootTrajectory(phi, stride_reach, stride_push, lift_max,
                                         strafe_reach, strafe_push,
                                         &dz_raw, &dx_raw, &lift_raw, &ankle_tilt_deg);

          if (stair_grade > 0.10f) {
            if (phi < 1.15f * PI) {
              float u = phi / (1.15f * PI);
              if (u > 0.65f) {
                float peel_t = (u - 0.65f) / 0.35f;
                ankle_tilt_deg -= stair_grade * 14.0f * (peel_t * peel_t);
              }
            }
          } else if (stair_grade < -0.10f) {
            if (phi < 1.15f * PI) {
              float u = phi / (1.15f * PI);
              if (u < 0.28f) {
                float roll_t = u / 0.28f;
                ankle_tilt_deg = Lerp(-15.0f * (-stair_grade), 0.0f, roll_t * (2.0f - roll_t));
              }
            } else {
              ankle_tilt_deg = -15.0f * (-stair_grade);
            }
          }

          if (fabsf(stair_grade) > 0.10f && is_moving) {
            float step_depth = 0.28f;
            float rel_z = fmodf(dz_raw + 10.0f * step_depth, step_depth);
            if (rel_z < 0.05f) {
              dz_raw += (stair_grade > 0.0f ? -0.035f : 0.035f);
            } else if (rel_z > step_depth - 0.05f) {
              dz_raw += (stair_grade > 0.0f ? 0.035f : -0.035f);
            }
          }

          if (!is_moving && fabsf(stair_grade) > 0.10f) {
            float stagger_mag = 0.16f * Clamp(fabsf(stair_grade), 0.0f, 1.0f) * (1.0f - gait_weight * 0.5f);
            bool is_lead = (leg == stair_lead_foot);
            float leg_stagger = is_lead ? +stagger_mag : -stagger_mag;
            if (stair_grade < 0.0f) leg_stagger = -leg_stagger;
            dz_raw += leg_stagger;
          }

          float lat_x = (leg == 0) ? (0.0789f + dx_raw) : (-0.0789f + dx_raw);
          Vector3 foot_local = { lat_x, 0.0f, dz_raw };
          Vector3 foot_world = Vector3Transform(foot_local, bodyMat);

          float ground_y = (is_arena_mode >= 2)
              ? GetGroundHeight(foot_world.x, foot_world.z, current_cam.y, 0.60f)
              : (body_y + crouch_drop - pelvis_bob);
          float ground_scale = (is_moving || crouch_lerp > 0.02f || fabsf(stair_grade) > 0.08f) ? 1.0f : 0.85f;
          float raw_delta = fmaxf(-0.45f, fminf(0.48f, ground_y - body_pos.y)) * ground_scale;

          float *sm_ground = (leg == 0) ? &sm_ground_l : &sm_ground_r;
          *sm_ground = Lerp(*sm_ground, raw_delta, 25.0f * dt);

          float target_y = ankle_y + (*sm_ground) + lift_raw;
          float dy = target_y - hip_y;
          float dz = dz_raw;

          float h_pitch, k_pitch, a_pitch;
          SolveAnatomicalLegIK(dy, dz, L1, L2, &h_pitch, &k_pitch, &a_pitch);

          a_pitch += (ankle_tilt_deg * DEG2RAD) * gait_weight;

          Vector3 H_bind = sayoriModel.skeleton.bindPose[hip_b].translation;
          Vector3 K_bind = sayoriModel.skeleton.bindPose[knee_b].translation;
          Vector3 A_bind = sayoriModel.skeleton.bindPose[ankle_b].translation;

          Quaternion H_bind_rot = sayoriModel.skeleton.bindPose[hip_b].rotation;
          Quaternion K_bind_rot = sayoriModel.skeleton.bindPose[knee_b].rotation;
          Quaternion A_bind_rot = sayoriModel.skeleton.bindPose[ankle_b].rotation;

          Vector3 offset_knee = Vector3Subtract(K_bind, H_bind);
          Vector3 offset_ankle = Vector3Subtract(A_bind, K_bind);

          Quaternion q_hip = QuaternionMultiply(H_bind_rot,
              QuaternionMultiply(QuaternionFromAxisAngle((Vector3){0, 0, 1}, -dx_raw * 1.5f),
                                 QuaternionFromAxisAngle((Vector3){1, 0, 0}, h_pitch)));
          Quaternion q_knee = QuaternionMultiply(q_hip, QuaternionFromAxisAngle((Vector3){1, 0, 0}, k_pitch));
          Quaternion q_ankle = QuaternionMultiply(q_knee, QuaternionFromAxisAngle((Vector3){1, 0, 0}, a_pitch));

          Vector3 pos_hip = H_bind;
          Vector3 pos_knee = Vector3Add(pos_hip, Vector3RotateByQuaternion(offset_knee, q_hip));
          Vector3 pos_ankle = Vector3Add(pos_knee, Vector3RotateByQuaternion(offset_ankle, q_knee));

          if (leg == 0) { pos_hip_l = pos_hip; pos_knee_l = pos_knee; }
          else { pos_hip_r = pos_hip; pos_knee_r = pos_knee; }

          sayori_anim.keyframePoses[0][hip_b].translation = pos_hip;
          sayori_anim.keyframePoses[0][hip_b].rotation = q_hip;

          sayori_anim.keyframePoses[0][knee_b].translation = pos_knee;
          sayori_anim.keyframePoses[0][knee_b].rotation = q_knee;

          sayori_anim.keyframePoses[0][ankle_b].translation = pos_ankle;
          sayori_anim.keyframePoses[0][ankle_b].rotation = q_ankle;

          if (toe_b >= 0) {
            Vector3 T_bind = sayoriModel.skeleton.bindPose[toe_b].translation;
            Quaternion T_bind_rot = sayoriModel.skeleton.bindPose[toe_b].rotation;
            Vector3 offset_toe = Vector3Subtract(T_bind, A_bind);

            Quaternion q_toe_bend = QuaternionFromAxisAngle((Vector3){1, 0, 0}, -ankle_tilt_deg * DEG2RAD * gait_weight);
            Quaternion q_toe = QuaternionMultiply(q_ankle, QuaternionMultiply(T_bind_rot, q_toe_bend));
            Vector3 pos_toe = Vector3Add(pos_ankle, Vector3RotateByQuaternion(offset_toe, q_ankle));

            sayori_anim.keyframePoses[0][toe_b].translation = pos_toe;
            sayori_anim.keyframePoses[0][toe_b].rotation = q_toe;
          }
        }

        if (s_knee_l >= 0 && s_knee_r >= 0 && skirt_cloth_inited) {
          float sim_dt = fminf(dt, 0.033f);
          Vector3 gravity = (Vector3){ 0.0f, -9.81f, 0.0f };

          Quaternion q_pelvis = QuaternionIdentity();
          if (s_lower_body >= 0) {
            q_pelvis = QuaternionMultiply(
              QuaternionFromAxisAngle((Vector3){0, 1, 0}, hip_twist_y),
              QuaternionFromAxisAngle((Vector3){0, 0, 1}, pelvic_tilt_z)
            );
          }
          Vector3 waist_center = (Vector3){ 0.0f, 0.96f, 0.0f };
          for (int c = 0; c < 11; c++) {
            int b0 = s_skirt_bones[c][0];
            if (b0 >= 0) {
              Vector3 bp0 = sayoriModel.skeleton.bindPose[b0].translation;
              Vector3 offset = Vector3Subtract(bp0, waist_center);
              Vector3 rotated_offset = Vector3RotateByQuaternion(offset, q_pelvis);
              skirt_particles_pos[c][0] = Vector3Add(waist_center, rotated_offset);
              skirt_particles_prev[c][0] = skirt_particles_pos[c][0];
            }
          }

          const float damping = 0.88f;
          for (int c = 0; c < 11; c++) {
            for (int l = 1; l < 10; l++) {
              Vector3 p = skirt_particles_pos[c][l];
              Vector3 p_prev = skirt_particles_prev[c][l];
              Vector3 vel = Vector3Scale(Vector3Subtract(p, p_prev), damping);

              Vector3 next_p = Vector3Add(p, vel);
              next_p = Vector3Add(next_p, Vector3Scale(gravity, sim_dt * sim_dt));

              int b = s_skirt_bones[c][l];
              if (b >= 0) {
                Vector3 bp = sayoriModel.skeleton.bindPose[b].translation;
                Vector3 rest_target = Vector3Add(waist_center, Vector3RotateByQuaternion(Vector3Subtract(bp, waist_center), q_pelvis));
                next_p = Vector3Lerp(next_p, rest_target, 0.05f);
              }

              skirt_particles_prev[c][l] = p;
              skirt_particles_pos[c][l] = next_p;
            }
          }

          bool is_crouching = (crouch_lerp > 0.35f);
          for (int iter = 0; iter < 6; iter++) {
            for (int c = 0; c < 11; c++) {
              for (int l = 1; l < 10; l++) {
                float link_u = (float)l / 9.0f;
                float r = is_crouching ? Lerp(0.108f, 0.102f, link_u) : Lerp(0.096f, 0.088f, link_u);
                ResolveThighClothCollision(&skirt_particles_pos[c][l], pos_hip_l, pos_knee_l, r, c, true);
                ResolveThighClothCollision(&skirt_particles_pos[c][l], pos_hip_r, pos_knee_r, r, c, false);
              }
            }

            for (int c = 0; c < 11; c++) {
              for (int l = 1; l < 10; l++) {
                Vector3 p_parent = skirt_particles_pos[c][l - 1];
                Vector3 *p_child = &skirt_particles_pos[c][l];
                Vector3 delta = Vector3Subtract(*p_child, p_parent);
                float dist = Vector3Length(delta);
                float target_len = skirt_vert_len[c][l];
                if (dist > 1e-4f && target_len > 1e-4f) {
                  float diff = (dist - target_len) / dist;
                  *p_child = Vector3Subtract(*p_child, Vector3Scale(delta, diff));
                }
              }
            }

            for (int l = 1; l < 10; l++) {
              for (int c = 0; c < 11; c++) {
                int c_next = (c + 1) % 11;
                Vector3 *p1 = &skirt_particles_pos[c][l];
                Vector3 *p2 = &skirt_particles_pos[c_next][l];
                Vector3 delta = Vector3Subtract(*p2, *p1);
                float dist = Vector3Length(delta);
                float rest_len = skirt_horiz_len[c][l];
                if (rest_len > 1e-4f) {
                  if (dist < rest_len * 0.95f) {
                    float diff = (dist - rest_len * 0.95f) / (dist > 1e-4f ? dist : 1.0f);
                    Vector3 push = Vector3Scale(delta, diff * 0.5f);
                    *p1 = Vector3Add(*p1, push);
                    *p2 = Vector3Subtract(*p2, push);
                  }
                  if (dist > rest_len * 1.30f) {
                    float diff = (dist - rest_len * 1.30f) / (dist > 1e-4f ? dist : 1.0f);
                    Vector3 pull = Vector3Scale(delta, diff * 0.5f);
                    *p1 = Vector3Add(*p1, pull);
                    *p2 = Vector3Subtract(*p2, pull);
                  }
                }
              }
            }
          }

          for (int c = 0; c < 11; c++) {
            for (int l = 0; l < 10; l++) {
              int b = s_skirt_bones[c][l];
              if (b < 0) continue;

              sayori_anim.keyframePoses[0][b].translation = skirt_particles_pos[c][l];

              if (l < 9) {
                int next_b = s_skirt_bones[c][l + 1];
                if (next_b >= 0) {
                  Vector3 v_bind = Vector3Subtract(sayoriModel.skeleton.bindPose[next_b].translation, sayoriModel.skeleton.bindPose[b].translation);
                  Vector3 v_cur = Vector3Subtract(skirt_particles_pos[c][l + 1], skirt_particles_pos[c][l]);
                  Quaternion q_align = QuaternionFromVector3ToVector3(Vector3Normalize(v_bind), Vector3Normalize(v_cur));
                  sayori_anim.keyframePoses[0][b].rotation = QuaternionMultiply(q_align, sayoriModel.skeleton.bindPose[b].rotation);
                }
              } else {
                int prev_b = s_skirt_bones[c][l - 1];
                if (prev_b >= 0) sayori_anim.keyframePoses[0][b].rotation = sayori_anim.keyframePoses[0][prev_b].rotation;
              }
            }
          }
        }

        if (cur_wp == 1) {
          bool is_crouching = (crouch_lerp > 0.35f);
          Vector3 arm_base_r = (s_chain_r.arm >= 0 && s_chain_r.arm < 256) ? spine_state.posed_pos[s_chain_r.arm] : sayoriModel.skeleton.bindPose[s_chain_r.arm].translation;
          Vector3 sh_base_r  = (s_chain_r.sh >= 0 && s_chain_r.sh < 256)   ? spine_state.posed_pos[s_chain_r.sh]  : sayoriModel.skeleton.bindPose[s_chain_r.sh].translation;
          Vector3 arm_base_l = (s_chain_l.arm >= 0 && s_chain_l.arm < 256) ? spine_state.posed_pos[s_chain_l.arm] : sayoriModel.skeleton.bindPose[s_chain_l.arm].translation;
          Vector3 sh_base_l  = (s_chain_l.sh >= 0 && s_chain_l.sh < 256)   ? spine_state.posed_pos[s_chain_l.sh]  : sayoriModel.skeleton.bindPose[s_chain_l.sh].translation;
          Quaternion q_spine = (s_upper_body2 >= 0 && s_upper_body2 < 256) ? spine_state.accum_rot[s_upper_body2] : QuaternionIdentity();

          Vector3 chest_center = (s_upper_body2 >= 0 && s_upper_body2 < 256)
              ? spine_state.posed_pos[s_upper_body2]
              : sayoriModel.skeleton.bindPose[s_upper_body2].translation;
          chest_center.y += 0.02f;
          chest_center.z -= 0.025f;

          float jog_blend = sprint_sul_t;
          SolveSayoriWorldArmIK(&sayori_anim, sayoriModel, invBodyMat, gunWorldMat, gunAnchor, &s_chain_r, true,  wall_lift_t, is_crouching, current_cam.pitch,
                                arm_base_r, sh_base_r, q_spine, chest_center, 0.0f, 0.0f, 0.0f, 0.0f);
          SolveSayoriWorldArmIK(&sayori_anim, sayoriModel, invBodyMat, gunWorldMat, gunAnchor, &s_chain_l, false, wall_lift_t, is_crouching, current_cam.pitch,
                                arm_base_l, sh_base_l, q_spine, chest_center, support_shear_z.pos, support_shear_yaw.pos, jog_blend, gait_cycle);

          ApplySayoriHandGrip(&sayori_anim, sayoriModel, s_chain_r.wrist, s_chain_l.wrist,
                              &s_fingers_r, &s_fingers_l, cur_wp, trigger_travel, wall_lift_t, (k_shift || was_sprint_active || sprint_sul_t > 0.35f), jog_blend);
        } else {
          if (s_arm_l >= 0) sayori_anim.keyframePoses[0][s_arm_l].rotation = QuaternionMultiply(sayori_anim.keyframePoses[0][s_arm_l].rotation, QuaternionFromAxisAngle((Vector3){0, 0, 1}, -70.0f * DEG2RAD));
          if (s_arm_r >= 0) sayori_anim.keyframePoses[0][s_arm_r].rotation = QuaternionMultiply(sayori_anim.keyframePoses[0][s_arm_r].rotation, QuaternionFromAxisAngle((Vector3){0, 0, 1}, 70.0f * DEG2RAD));
        }

        UpdateModelAnimation(sayoriModel, sayori_anim, 0);
      }

      float body_rim_pow = 0.08f;
      SetShaderValue(cel_shader, cel_rim_pow_loc, &body_rim_pow, SHADER_UNIFORM_FLOAT);
      for (int m = 0; m < sayoriModel.meshCount; m++) {
        bool is_head = (m <= 14 || (m >= 17 && m <= 20) || m >= 31);
        if (is_head) continue;
        DrawMesh(sayoriModel.meshes[m], sayoriModel.materials[sayoriModel.meshMaterial[m]], bodyMat);
      }

      for (int i = 0; i < MAX_SPARKS; i++) {
        if (world_sparks[i].life > 0.0f) {
          world_sparks[i].life -= dt;
          world_sparks[i].vel.y -= 9.8f * dt;
          world_sparks[i].pos = Vector3Add(world_sparks[i].pos, Vector3Scale(world_sparks[i].vel, dt));
          float alpha = Clamp(world_sparks[i].life / world_sparks[i].max_life, 0.0f, 1.0f);
          Color c = world_sparks[i].color;
          c.a = (unsigned char)(alpha * 255);
          Vector3 p1 = world_sparks[i].pos;
          Vector3 p2 = Vector3Subtract(p1, Vector3Scale(world_sparks[i].vel, 0.02f));
          DrawLine3D(p1, p2, c);
        }
      }

      if (cur_wp == 1) {
        if (has_m1911_anim) {
          for (int b = 0; b < (int)m1911_anim.boneCount; b++) {
            m1911_anim.keyframePoses[0][b] = weaponModels[1].skeleton.bindPose[b];
          }
          if (m1911_b_slide < m1911_anim.boneCount) {
            m1911_anim.keyframePoses[0][m1911_b_slide].translation.z += 0.038f * slide_blowback;
          }
          if (m1911_b_hammer < m1911_anim.boneCount) {
            m1911_anim.keyframePoses[0][m1911_b_hammer].rotation =
                QuaternionFromAxisAngle((Vector3){1, 0, 0}, (48.0f + 7.0f * slide_blowback) * DEG2RAD);
          }
          if (m1911_b_trigger < m1911_anim.boneCount) {
            m1911_anim.keyframePoses[0][m1911_b_trigger].translation.z += 0.0035f * trigger_travel;
          }
          if (m1911_b_grip_safety < m1911_anim.boneCount) {
            m1911_anim.keyframePoses[0][m1911_b_grip_safety].rotation =
                QuaternionFromAxisAngle((Vector3){1, 0, 0}, 5.0f * DEG2RAD);
          }
          if (m1911_b_barrel < m1911_anim.boneCount) {
            m1911_anim.keyframePoses[0][m1911_b_barrel].translation.z += 0.0030f * slide_blowback;
            m1911_anim.keyframePoses[0][m1911_b_barrel].rotation =
                QuaternionFromAxisAngle((Vector3){1, 0, 0}, (-1.3f * slide_blowback) * DEG2RAD);
          }
          UpdateModelAnimation(weaponModels[1], m1911_anim, 0);
        }

        float gun_rim_pow = 0.09f;
        SetShaderValue(cel_shader, cel_rim_pow_loc, &gun_rim_pow, SHADER_UNIFORM_FLOAT);
        Matrix gunDrawMat = (fabsf(wp_scale - 1.0f) > 1e-4f)
            ? MatrixMultiply(MatrixScale(wp_scale, wp_scale, wp_scale), gunWorldMat)
            : gunWorldMat;
        for (int m = 0; m < weaponModels[1].meshCount; m++) {
          DrawMesh(weaponModels[1].meshes[m], weaponModels[1].materials[weaponModels[1].meshMaterial[m]], gunDrawMat);
        }
      }
    }

    EndMode3D();

    if (!(cur_wp == 1 && has_sayori_model)) {
      Camera3D weapon_cam = {0};
      weapon_cam.position = (Vector3){0.0f, 0.0f, 0.0f};
      weapon_cam.target = (Vector3){0.0f, 0.0f, -1.0f};
      weapon_cam.up = (Vector3){0.0f, 1.0f, 0.0f};
      weapon_cam.fovy = camera.fovy;
      weapon_cam.projection = CAMERA_PERSPECTIVE;

      rlSetClipPlanes(0.02, 100.0);
      BeginMode3D(weapon_cam);
      glad_glClear(0x00000100);

      Vector3 hip_pos = {
          base_hip_x + bob_x + breath_x + lag_trans_x.pos,
          base_hip_y + bob_y + breath_y + lag_trans_y.pos,
          base_hip_z + recoil_offset_z
      };
      Vector3 ads_pos = {
          base_ads_x + (bob_x + breath_x) * 0.15f + lag_trans_x.pos * 0.20f,
          base_ads_y + (bob_y + breath_y) * 0.15f + lag_trans_y.pos * 0.20f,
          base_ads_z + recoil_offset_z
      };
      Vector3 wp_pos = Vector3Lerp(hip_pos, ads_pos, ads_lerp);

      float wp_pitch = base_pitch + recoil_pitch + lag_pitch.pos;
      float wp_yaw = base_yaw + lag_yaw.pos;
      float wp_roll = base_roll + lag_roll.pos;

      Matrix rotX = MatrixRotateX(wp_pitch * DEG2RAD);
      Matrix rotY = MatrixRotateY(wp_yaw * DEG2RAD);
      Matrix rotZ = MatrixRotateZ(wp_roll * DEG2RAD);
      weaponModels[cur_wp].transform = MatrixMultiply(MatrixMultiply(rotX, rotY), rotZ);

      Vector3 wp_light_dir = {0.50f, 0.85f, 0.60f};
      Vector3 wp_light_col = {1.15f, 1.10f, 1.05f};
      Vector3 wp_amb_col = {0.72f, 0.74f, 0.80f};
      Vector3 wp_view_pos = {0.0f, 0.0f, 0.0f};
      float wp_rim_pow = 0.10f;
      Vector3 wp_rim_col = {0.80f, 0.90f, 1.0f};

      SetShaderValue(cel_shader, cel_light_dir_loc, &wp_light_dir, SHADER_UNIFORM_VEC3);
      SetShaderValue(cel_shader, cel_light_col_loc, &wp_light_col, SHADER_UNIFORM_VEC3);
      SetShaderValue(cel_shader, cel_amb_col_loc, &wp_amb_col, SHADER_UNIFORM_VEC3);
      SetShaderValue(cel_shader, cel_view_pos_loc, &wp_view_pos, SHADER_UNIFORM_VEC3);
      SetShaderValue(cel_shader, cel_rim_pow_loc, &wp_rim_pow, SHADER_UNIFORM_FLOAT);
      SetShaderValue(cel_shader, cel_rim_col_loc, &wp_rim_col, SHADER_UNIFORM_VEC3);

      SetShaderValue(dyn_light_shader, dyn_view_pos_loc, &wp_view_pos, SHADER_UNIFORM_VEC3);
      SetShaderValue(dyn_light_shader, dyn_sun_dir_loc, &wp_light_dir, SHADER_UNIFORM_VEC3);
      SetShaderValue(dyn_light_shader, dyn_sun_col_loc, &wp_light_col, SHADER_UNIFORM_VEC3);
      SetShaderValue(dyn_light_shader, dyn_amb_col_loc, &wp_amb_col, SHADER_UNIFORM_VEC3);
      SetShaderValue(dyn_light_shader, dyn_flat_norm_loc, &flat_norm_char, SHADER_UNIFORM_INT);

      DrawModelEx(weaponModels[cur_wp], wp_pos, (Vector3){0,1,0}, 0.0f, (Vector3){wp_scale, wp_scale, wp_scale}, WHITE);

      EndMode3D();
    }
    rlSetClipPlanes(0.02, 4000.0);
    rlEnableDepthTest();

    EndTextureMode();

    BeginTextureMode(final_target);
    ClearBackground(BLACK);
    
    float current_time = (float)GetTime();
    SetShaderValue(lighting_shader, time_loc, &current_time, SHADER_UNIFORM_FLOAT);
    SetShaderValue(lighting_shader, tod_loc, &current_cam.time_of_day, SHADER_UNIFORM_FLOAT);
    SetShaderValue(lighting_shader, sh_loc, &current_cam.shadows_enabled, SHADER_UNIFORM_INT);
    SetShaderValue(lighting_shader, lq_loc, &current_cam.lighting_quality, SHADER_UNIFORM_INT);
    
    SetShaderValue(lighting_shader, bloom_loc, &current_input.bloom_enabled, SHADER_UNIFORM_INT);
    SetShaderValue(lighting_shader, clouds_loc, &current_input.volumetric_clouds, SHADER_UNIFORM_INT);
    SetShaderValue(lighting_shader, weather_loc, &current_input.rain_intensity, SHADER_UNIFORM_FLOAT);
    SetShaderValue(lighting_shader, yaw_loc, &current_cam.yaw, SHADER_UNIFORM_FLOAT);
    SetShaderValue(lighting_shader, pitch_loc, &current_cam.pitch, SHADER_UNIFORM_FLOAT);
    
    float aspect = (float)target.texture.width / (float)target.texture.height;
    SetShaderValue(lighting_shader, aspect_loc, &aspect, SHADER_UNIFORM_FLOAT);
    
    Vector3 cam_pos_val = {current_cam.x, target_height, current_cam.z};
    SetShaderValue(lighting_shader, cam_pos_loc, &cam_pos_val, SHADER_UNIFORM_VEC3);
    
    SetShaderValue(lighting_shader, fov_loc, &camera.fovy, SHADER_UNIFORM_FLOAT);
    float flash_val = (muzzle_flash_timer > 0.0f) ? (muzzle_flash_timer / 0.08f) : 0.0f;
    SetShaderValue(lighting_shader, flash_loc, &flash_val, SHADER_UNIFORM_FLOAT);
    
    float fl_val = current_input.flashlight_active ? 1.0f : 0.0f;
    SetShaderValue(lighting_shader, fl_loc, &fl_val, SHADER_UNIFORM_FLOAT);
    
    static float smoothed_vel_x = 0.0f;
    static float smoothed_vel_y = 0.0f;
    smoothed_vel_x = Lerp(smoothed_vel_x, mdx, 15.0f * dt);
    smoothed_vel_y = Lerp(smoothed_vel_y, mdy, 15.0f * dt);
    Vector2 cam_velocity = (Vector2){ smoothed_vel_x, smoothed_vel_y };

    Vector2 fl_offset = (Vector2){
        (bob_x * 2.5f + lag_yaw.pos * 0.003f) * (1.0f - ads_lerp * 0.7f),
        (-bob_y * 2.5f + lag_pitch.pos * 0.003f) * (1.0f - ads_lerp * 0.7f)
    };

    SetShaderValue(lighting_shader, fl_offset_loc, &fl_offset, SHADER_UNIFORM_VEC2);
    SetShaderValue(lighting_shader, mb_strength_loc, &current_input.motion_blur_strength, SHADER_UNIFORM_FLOAT);
    SetShaderValue(lighting_shader, cam_vel_loc, &cam_velocity, SHADER_UNIFORM_VEC2);
    
    BeginShaderMode(lighting_shader);
    DrawTextureRec(target.texture, 
                   (Rectangle){0, 0, (float)target.texture.width,
                               -(float)target.texture.height},
                   (Vector2){0, 0}, WHITE);
    EndShaderMode();

    EndTextureMode();

    Image img = LoadImageFromTexture(final_target.texture);
    ImageFlipVertical(&img);
    ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);

    int dataSize = img.width * img.height * 4;
    int msg_type = 0;
    fwrite(&msg_type, sizeof(int), 1, stdout);
    fwrite(&dataSize, sizeof(int), 1, stdout);
    fwrite(img.data, 1, dataSize, stdout);
    fflush(stdout);
    UnloadImage(img);

    BeginDrawing();
    ClearBackground(BLACK);
    DrawTextureRec(final_target.texture,
                   (Rectangle){0, 0, (float)final_target.texture.width,
                               -(float)final_target.texture.height},
                   (Vector2){0, 0}, WHITE);
    EndDrawing();
  }

  for (int i = 0; i < 9; i++) {
    UnloadTexture(wall_textures[i]);
  }
  UnloadTexture(floor_tex);
  for (int i = 0; i < 16; i++) {
    UnloadTexture(sprite_tex[i]);
  }
  UnloadModel(cubeModel);
  UnloadModel(floorModel);
  if (has_m1911_anim && m1911_anim.keyframePoses) {
    if (m1911_anim.keyframePoses[0]) free(m1911_anim.keyframePoses[0]);
    free(m1911_anim.keyframePoses);
  }
  for (int i = 0; i < 4; i++) {
    UnloadModel(weaponModels[i]);
  }
  if (has_world3d_model) {
    if (world3d_mesh_boxes != NULL) {
      free(world3d_mesh_boxes);
      world3d_mesh_boxes = NULL;
    }
    UnloadModel(world3DModel);
    FreeWorldGrid();
  }
  if (has_sayori_model) {
    if (sayori_anim.keyframePoses) {
      if (sayori_anim.keyframePoses[0]) free(sayori_anim.keyframePoses[0]);
      free(sayori_anim.keyframePoses);
    }
    UnloadModel(sayoriModel);
  }
  UnloadShader(lighting_shader);
  UnloadShader(cel_shader);
  UnloadShader(dyn_light_shader);
  UnloadRenderTexture(target);
  UnloadRenderTexture(final_target);
  CloseWindow();
  if (dynamic_map)
    free(dynamic_map);
  return 0;
}
