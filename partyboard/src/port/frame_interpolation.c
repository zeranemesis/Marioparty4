#include "port/frame_interpolation.h"

#include "game/hu3d.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

typedef struct ModelSnapshot {
    const void *identity;
    uintptr_t allocation;
    u32 generation;
    HuVecF pos;
    HuVecF rot;
    HuVecF scale;
    Mtx mtx;
} ModelSnapshot;

typedef struct PBQuaternion {
    float x;
    float y;
    float z;
    float w;
} PBQuaternion;

typedef struct AffineTransform {
    HuVecF translation;
    HuVecF scale;
    PBQuaternion rotation;
} AffineTransform;

typedef struct CameraSnapshot {
    bool active;
    u32 generation;
    HU3DCAMERA camera;
} CameraSnapshot;

typedef struct SpriteSnapshot {
    const void *identity;
    u32 generation;
    s16 attr;
    HuVec2f pos;
    float zRot;
    HuVec2f scale;
    float alpha;
    u8 r;
    u8 g;
    u8 b;
} SpriteSnapshot;

typedef struct SpriteGroupSnapshot {
    const s16 *identity;
    s16 capacity;
    u32 generation;
    HuVec2f pos;
    float zRot;
    HuVec2f scale;
    HuVec2f center;
} SpriteGroupSnapshot;

static ModelSnapshot s_previousModels[HU3D_MODEL_MAX];
static ModelSnapshot s_currentModels[HU3D_MODEL_MAX];
static CameraSnapshot s_previousCameras[HU3D_CAM_MAX];
static CameraSnapshot s_currentCameras[HU3D_CAM_MAX];
static SpriteSnapshot s_previousSprites[HUSPR_MAX];
static SpriteSnapshot s_currentSprites[HUSPR_MAX];
static SpriteGroupSnapshot s_previousSpriteGroups[HUSPR_GRP_MAX];
static SpriteGroupSnapshot s_currentSpriteGroups[HUSPR_GRP_MAX];
static u32 s_modelGenerations[HU3D_MODEL_MAX];
static u32 s_cameraGenerations[HU3D_CAM_MAX];
static u32 s_spriteGenerations[HUSPR_MAX];
static u32 s_spriteGroupGenerations[HUSPR_GRP_MAX];

static bool s_initialized;
static bool s_enabled;
static float s_step;

static float lerp_float(float previous, float current, float step)
{
    return previous + (current - previous) * step;
}

static float lerp_angle(float previous, float current, float step)
{
    float delta = fmodf(current - previous + 180.0f, 360.0f);
    if (delta < 0.0f) {
        delta += 360.0f;
    }
    delta -= 180.0f;
    return previous + delta * step;
}

static void lerp_vector(HuVecF *out, const HuVecF *previous, const HuVecF *current, float step)
{
    out->x = lerp_float(previous->x, current->x, step);
    out->y = lerp_float(previous->y, current->y, step);
    out->z = lerp_float(previous->z, current->z, step);
}

static void lerp_vector2(HuVec2f *out, const HuVec2f *previous, const HuVec2f *current, float step)
{
    out->x = lerp_float(previous->x, current->x, step);
    out->y = lerp_float(previous->y, current->y, step);
}

static float matrix_column_length(const Mtx matrix, s32 column)
{
    return sqrtf(matrix[0][column] * matrix[0][column] +
                 matrix[1][column] * matrix[1][column] +
                 matrix[2][column] * matrix[2][column]);
}

static bool decompose_affine_matrix(const Mtx matrix, AffineTransform *transform)
{
    float rotation[3][3];
    float determinant;
    float dot01;
    float dot02;
    float dot12;
    float trace;
    float root;
    s32 row;

    transform->translation.x = matrix[0][3];
    transform->translation.y = matrix[1][3];
    transform->translation.z = matrix[2][3];
    transform->scale.x = matrix_column_length(matrix, 0);
    transform->scale.y = matrix_column_length(matrix, 1);
    transform->scale.z = matrix_column_length(matrix, 2);
    if (transform->scale.x < 0.000001f || transform->scale.y < 0.000001f ||
        transform->scale.z < 0.000001f) {
        return false;
    }

    determinant =
        matrix[0][0] * (matrix[1][1] * matrix[2][2] - matrix[1][2] * matrix[2][1]) -
        matrix[0][1] * (matrix[1][0] * matrix[2][2] - matrix[1][2] * matrix[2][0]) +
        matrix[0][2] * (matrix[1][0] * matrix[2][1] - matrix[1][1] * matrix[2][0]);
    if (determinant < 0.0f) {
        transform->scale.x = -transform->scale.x;
    }

    for (row = 0; row < 3; ++row) {
        rotation[row][0] = matrix[row][0] / transform->scale.x;
        rotation[row][1] = matrix[row][1] / transform->scale.y;
        rotation[row][2] = matrix[row][2] / transform->scale.z;
    }

    dot01 = rotation[0][0] * rotation[0][1] + rotation[1][0] * rotation[1][1] +
            rotation[2][0] * rotation[2][1];
    dot02 = rotation[0][0] * rotation[0][2] + rotation[1][0] * rotation[1][2] +
            rotation[2][0] * rotation[2][2];
    dot12 = rotation[0][1] * rotation[0][2] + rotation[1][1] * rotation[1][2] +
            rotation[2][1] * rotation[2][2];
    if (fabsf(dot01) > 0.001f || fabsf(dot02) > 0.001f || fabsf(dot12) > 0.001f) {
        return false;
    }

    trace = rotation[0][0] + rotation[1][1] + rotation[2][2];
    if (trace > 0.0f) {
        root = sqrtf(trace + 1.0f) * 2.0f;
        transform->rotation.w = 0.25f * root;
        transform->rotation.x = (rotation[2][1] - rotation[1][2]) / root;
        transform->rotation.y = (rotation[0][2] - rotation[2][0]) / root;
        transform->rotation.z = (rotation[1][0] - rotation[0][1]) / root;
    } else if (rotation[0][0] > rotation[1][1] && rotation[0][0] > rotation[2][2]) {
        root = sqrtf(1.0f + rotation[0][0] - rotation[1][1] - rotation[2][2]) * 2.0f;
        transform->rotation.w = (rotation[2][1] - rotation[1][2]) / root;
        transform->rotation.x = 0.25f * root;
        transform->rotation.y = (rotation[0][1] + rotation[1][0]) / root;
        transform->rotation.z = (rotation[0][2] + rotation[2][0]) / root;
    } else if (rotation[1][1] > rotation[2][2]) {
        root = sqrtf(1.0f + rotation[1][1] - rotation[0][0] - rotation[2][2]) * 2.0f;
        transform->rotation.w = (rotation[0][2] - rotation[2][0]) / root;
        transform->rotation.x = (rotation[0][1] + rotation[1][0]) / root;
        transform->rotation.y = 0.25f * root;
        transform->rotation.z = (rotation[1][2] + rotation[2][1]) / root;
    } else {
        root = sqrtf(1.0f + rotation[2][2] - rotation[0][0] - rotation[1][1]) * 2.0f;
        transform->rotation.w = (rotation[1][0] - rotation[0][1]) / root;
        transform->rotation.x = (rotation[0][2] + rotation[2][0]) / root;
        transform->rotation.y = (rotation[1][2] + rotation[2][1]) / root;
        transform->rotation.z = 0.25f * root;
    }
    return true;
}

static void interpolate_affine_matrix(Mtx out, const AffineTransform *previous,
                                      const AffineTransform *current, float step)
{
    PBQuaternion rotation;
    HuVecF translation;
    HuVecF scale;
    float dot = previous->rotation.x * current->rotation.x +
                previous->rotation.y * current->rotation.y +
                previous->rotation.z * current->rotation.z +
                previous->rotation.w * current->rotation.w;
    const float sign = dot < 0.0f ? -1.0f : 1.0f;
    float length;

    rotation.x = lerp_float(previous->rotation.x, current->rotation.x * sign, step);
    rotation.y = lerp_float(previous->rotation.y, current->rotation.y * sign, step);
    rotation.z = lerp_float(previous->rotation.z, current->rotation.z * sign, step);
    rotation.w = lerp_float(previous->rotation.w, current->rotation.w * sign, step);
    length = sqrtf(rotation.x * rotation.x + rotation.y * rotation.y +
                   rotation.z * rotation.z + rotation.w * rotation.w);
    if (length > 0.000001f) {
        rotation.x /= length;
        rotation.y /= length;
        rotation.z /= length;
        rotation.w /= length;
    }

    lerp_vector(&translation, &previous->translation, &current->translation, step);
    lerp_vector(&scale, &previous->scale, &current->scale, step);

    out[0][0] = (1.0f - 2.0f * (rotation.y * rotation.y + rotation.z * rotation.z)) * scale.x;
    out[1][0] = (2.0f * (rotation.x * rotation.y + rotation.z * rotation.w)) * scale.x;
    out[2][0] = (2.0f * (rotation.x * rotation.z - rotation.y * rotation.w)) * scale.x;
    out[0][1] = (2.0f * (rotation.x * rotation.y - rotation.z * rotation.w)) * scale.y;
    out[1][1] = (1.0f - 2.0f * (rotation.x * rotation.x + rotation.z * rotation.z)) * scale.y;
    out[2][1] = (2.0f * (rotation.y * rotation.z + rotation.x * rotation.w)) * scale.y;
    out[0][2] = (2.0f * (rotation.x * rotation.z + rotation.y * rotation.w)) * scale.z;
    out[1][2] = (2.0f * (rotation.y * rotation.z - rotation.x * rotation.w)) * scale.z;
    out[2][2] = (1.0f - 2.0f * (rotation.x * rotation.x + rotation.y * rotation.y)) * scale.z;
    out[0][3] = translation.x;
    out[1][3] = translation.y;
    out[2][3] = translation.z;
}

static u8 lerp_color(u8 previous, u8 current, float step)
{
    float value = lerp_float((float)previous, (float)current, step);
    if (value < 0.0f) {
        value = 0.0f;
    } else if (value > 255.0f) {
        value = 255.0f;
    }
    return (u8)(value + 0.5f);
}

static void snapshot_models(ModelSnapshot *destination)
{
    s32 i;
    for (i = 0; i < HU3D_MODEL_MAX; ++i) {
        HU3DMODEL *model = &Hu3DData[i];
        ModelSnapshot *snapshot = &destination[i];
        snapshot->identity = model->hsf;
        snapshot->allocation = model->mallocNo;
        snapshot->generation = s_modelGenerations[i];
        snapshot->pos = model->pos;
        snapshot->rot = model->rot;
        snapshot->scale = model->scale;
        MTXCopy(model->mtx, snapshot->mtx);
    }
}

static void snapshot_cameras(CameraSnapshot *destination)
{
    s32 i;
    for (i = 0; i < HU3D_CAM_MAX; ++i) {
        destination[i].active = Hu3DCamera[i].fov != -1.0f;
        destination[i].generation = s_cameraGenerations[i];
        destination[i].camera = Hu3DCamera[i];
    }
}

static void snapshot_sprites(SpriteSnapshot *destination)
{
    s32 i;
    for (i = 0; i < HUSPR_MAX; ++i) {
        HUSPRITE *sprite = &HuSprData[i];
        SpriteSnapshot *snapshot = &destination[i];
        snapshot->identity = sprite->data;
        snapshot->generation = s_spriteGenerations[i];
        snapshot->attr = sprite->attr;
        snapshot->pos = sprite->pos;
        snapshot->zRot = sprite->zRot;
        snapshot->scale = sprite->scale;
        snapshot->alpha = sprite->a;
        snapshot->r = sprite->r;
        snapshot->g = sprite->g;
        snapshot->b = sprite->b;
    }
}

static void snapshot_sprite_groups(SpriteGroupSnapshot *destination)
{
    s32 i;
    for (i = 0; i < HUSPR_GRP_MAX; ++i) {
        HUSPRGRP *group = &HuSprGrpData[i];
        SpriteGroupSnapshot *snapshot = &destination[i];
        snapshot->identity = group->members;
        snapshot->capacity = group->capacity;
        snapshot->generation = s_spriteGroupGenerations[i];
        snapshot->pos = group->pos;
        snapshot->zRot = group->zRot;
        snapshot->scale = group->scale;
        snapshot->center = group->center;
    }
}

void PartyBoard_FrameInterpolationBeginSimulation(void)
{
    if (!s_enabled) {
        return;
    }
    if (!s_initialized) {
        snapshot_models(s_currentModels);
        snapshot_cameras(s_currentCameras);
        snapshot_sprites(s_currentSprites);
        snapshot_sprite_groups(s_currentSpriteGroups);
        memcpy(s_previousModels, s_currentModels, sizeof(s_previousModels));
        memcpy(s_previousCameras, s_currentCameras, sizeof(s_previousCameras));
        memcpy(s_previousSprites, s_currentSprites, sizeof(s_previousSprites));
        memcpy(s_previousSpriteGroups, s_currentSpriteGroups, sizeof(s_previousSpriteGroups));
        s_initialized = true;
    } else {
        memcpy(s_previousModels, s_currentModels, sizeof(s_previousModels));
        memcpy(s_previousCameras, s_currentCameras, sizeof(s_previousCameras));
        memcpy(s_previousSprites, s_currentSprites, sizeof(s_previousSprites));
        memcpy(s_previousSpriteGroups, s_currentSpriteGroups, sizeof(s_previousSpriteGroups));
    }
}

void PartyBoard_FrameInterpolationEndSimulation(void)
{
    if (!s_enabled) {
        return;
    }
    snapshot_models(s_currentModels);
    snapshot_cameras(s_currentCameras);
    snapshot_sprites(s_currentSprites);
    snapshot_sprite_groups(s_currentSpriteGroups);
}

void PartyBoard_FrameInterpolationReset(void)
{
    /* A focus loss, frame-rate change, or long stall invalidates the temporal
     * relationship between the two snapshots. Re-seed them on the next tick. */
    s_initialized = false;
    s_step = 1.0f;
}

void PartyBoard_FrameInterpolationSetEnabled(bool enabled)
{
    if (enabled && !s_enabled) {
        s_initialized = false;
    }
    s_enabled = enabled;
}

void PartyBoard_FrameInterpolationSetStep(float step)
{
    if (step < 0.0f) {
        step = 0.0f;
    } else if (step > 1.0f) {
        /* Only blend between states that really happened. Global prediction
         * overshoots whenever motion stops or changes direction. */
        step = 1.0f;
    }
    s_step = step;
}

void PartyBoard_FrameInterpolationInvalidateModel(s16 modelId)
{
    if (modelId >= 0 && modelId < HU3D_MODEL_MAX) {
        ++s_modelGenerations[modelId];
    }
}

void PartyBoard_FrameInterpolationInvalidateCamera(s16 cameraId)
{
    if (cameraId >= 0 && cameraId < HU3D_CAM_MAX) {
        ++s_cameraGenerations[cameraId];
    }
}

void PartyBoard_FrameInterpolationInvalidateSprite(s16 spriteId)
{
    if (spriteId >= 0 && spriteId < HUSPR_MAX) {
        ++s_spriteGenerations[spriteId];
    }
}

void PartyBoard_FrameInterpolationInvalidateSpriteGroup(s16 groupId)
{
    if (groupId >= 0 && groupId < HUSPR_GRP_MAX) {
        ++s_spriteGroupGenerations[groupId];
    }
}

bool PartyBoard_FrameInterpolationModel(s16 modelId, HuVecF *pos, HuVecF *rot, HuVecF *scale, Mtx mtx)
{
    HU3DMODEL *model;
    const ModelSnapshot *previous;
    const ModelSnapshot *current;
    AffineTransform previousTransform;
    AffineTransform currentTransform;

    if (!s_enabled || !s_initialized || modelId < 0 || modelId >= HU3D_MODEL_MAX) {
        return false;
    }

    model = &Hu3DData[modelId];
    previous = &s_previousModels[modelId];
    current = &s_currentModels[modelId];
    if (model->hsf == NULL || previous->identity != current->identity ||
        current->identity != model->hsf || previous->allocation != current->allocation ||
        current->allocation != model->mallocNo ||
        previous->generation != current->generation ||
        current->generation != s_modelGenerations[modelId]) {
        return false;
    }

    lerp_vector(pos, &previous->pos, &current->pos, s_step);
    rot->x = lerp_angle(previous->rot.x, current->rot.x, s_step);
    rot->y = lerp_angle(previous->rot.y, current->rot.y, s_step);
    rot->z = lerp_angle(previous->rot.z, current->rot.z, s_step);
    lerp_vector(scale, &previous->scale, &current->scale, s_step);
    if (decompose_affine_matrix(previous->mtx, &previousTransform) &&
        decompose_affine_matrix(current->mtx, &currentTransform)) {
        /* Crossing a zero scale changes handedness and makes quaternion
         * interpolation undefined. Treat it as an authored discontinuity. */
        if (previousTransform.scale.x * currentTransform.scale.x > 0.0f &&
            previousTransform.scale.y * currentTransform.scale.y > 0.0f &&
            previousTransform.scale.z * currentTransform.scale.z > 0.0f) {
            interpolate_affine_matrix(mtx, &previousTransform, &currentTransform, s_step);
        }
    } else {
        /* Element-wise blending of an authored shear or projection matrix can
         * temporarily stop being a valid transform, visibly folding or
         * stretching geometry. Keep the authoritative current matrix. */
    }
    return true;
}

bool PartyBoard_FrameInterpolationCamera(s16 cameraId, HU3DCAMERA *camera)
{
    const CameraSnapshot *previous;
    const CameraSnapshot *current;

    if (!s_enabled || !s_initialized || cameraId < 0 || cameraId >= HU3D_CAM_MAX) {
        return false;
    }

    previous = &s_previousCameras[cameraId];
    current = &s_currentCameras[cameraId];
    if (!previous->active || !current->active || Hu3DCamera[cameraId].fov == -1.0f ||
        previous->generation != current->generation ||
        current->generation != s_cameraGenerations[cameraId]) {
        return false;
    }

    *camera = current->camera;
    camera->fov = lerp_float(previous->camera.fov, current->camera.fov, s_step);
    camera->nnear = lerp_float(previous->camera.nnear, current->camera.nnear, s_step);
    camera->ffar = lerp_float(previous->camera.ffar, current->camera.ffar, s_step);
    camera->aspect = lerp_float(previous->camera.aspect, current->camera.aspect, s_step);
    camera->upRot = lerp_angle(previous->camera.upRot, current->camera.upRot, s_step);
    lerp_vector(&camera->pos, &previous->camera.pos, &current->camera.pos, s_step);
    lerp_vector(&camera->up, &previous->camera.up, &current->camera.up, s_step);
    lerp_vector(&camera->target, &previous->camera.target, &current->camera.target, s_step);
    {
        const float upLength = sqrtf(camera->up.x * camera->up.x +
                                     camera->up.y * camera->up.y +
                                     camera->up.z * camera->up.z);
        if (upLength > 0.000001f) {
            camera->up.x /= upLength;
            camera->up.y /= upLength;
            camera->up.z /= upLength;
        }
    }
    return true;
}

bool PartyBoard_FrameInterpolationSprite(s16 spriteId, HUSPRITE *sprite)
{
    const SpriteSnapshot *previous;
    const SpriteSnapshot *current;
    HUSPRITE *liveSprite;

    if (!s_enabled || !s_initialized || spriteId < 0 || spriteId >= HUSPR_MAX) {
        return false;
    }

    liveSprite = &HuSprData[spriteId];
    previous = &s_previousSprites[spriteId];
    current = &s_currentSprites[spriteId];
    if (liveSprite->data == NULL || previous->identity != current->identity ||
        current->identity != liveSprite->data ||
        previous->generation != current->generation ||
        current->generation != s_spriteGenerations[spriteId] ||
        ((previous->attr ^ current->attr) & HUSPR_ATTR_FUNC) != 0) {
        return false;
    }

    lerp_vector2(&sprite->pos, &previous->pos, &current->pos, s_step);
    sprite->zRot = lerp_angle(previous->zRot, current->zRot, s_step);
    lerp_vector2(&sprite->scale, &previous->scale, &current->scale, s_step);
    sprite->a = lerp_float(previous->alpha, current->alpha, s_step);
    sprite->r = lerp_color(previous->r, current->r, s_step);
    sprite->g = lerp_color(previous->g, current->g, s_step);
    sprite->b = lerp_color(previous->b, current->b, s_step);
    return true;
}

bool PartyBoard_FrameInterpolationSpriteGroup(s16 groupId, HUSPRGRP *group)
{
    const SpriteGroupSnapshot *previous;
    const SpriteGroupSnapshot *current;
    HUSPRGRP *liveGroup;

    if (!s_enabled || !s_initialized || groupId < 0 || groupId >= HUSPR_GRP_MAX) {
        return false;
    }

    liveGroup = &HuSprGrpData[groupId];
    previous = &s_previousSpriteGroups[groupId];
    current = &s_currentSpriteGroups[groupId];
    if (liveGroup->capacity == 0 || previous->capacity != current->capacity ||
        current->capacity != liveGroup->capacity || previous->identity != current->identity ||
        current->identity != liveGroup->members ||
        previous->generation != current->generation ||
        current->generation != s_spriteGroupGenerations[groupId]) {
        return false;
    }

    lerp_vector2(&group->pos, &previous->pos, &current->pos, s_step);
    group->zRot = lerp_angle(previous->zRot, current->zRot, s_step);
    lerp_vector2(&group->scale, &previous->scale, &current->scale, s_step);
    lerp_vector2(&group->center, &previous->center, &current->center, s_step);
    return true;
}

bool PartyBoard_FrameInterpolationEnabled(void)
{
    return s_enabled && s_initialized;
}

float PartyBoard_FrameInterpolationStep(void)
{
    return s_step;
}
