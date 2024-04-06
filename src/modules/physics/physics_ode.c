#include "physics.h"
#include "util.h"
#include <ode/ode.h>
#include <stdatomic.h>
#include <stdlib.h>

struct World {
  uint32_t ref;
  dWorldID id;
  dSpaceID space;
  dJointGroupID contactGroup;
  arr_t(Shape*) overlaps;
  char* tags[MAX_TAGS];
  uint32_t masks[MAX_TAGS];
  Collider* head;
};

struct Collider {
  uint32_t ref;
  dBodyID body;
  World* world;
  Collider* prev;
  Collider* next;
  uint32_t tag;
  Shape* shape;
  arr_t(Joint*) joints;
  float friction;
  float restitution;
};

struct Shape {
  uint32_t ref;
  ShapeType type;
  dGeomID id;
  Collider* collider;
  void* vertices;
  void* indices;
  bool sensor;
};

struct Joint {
  uint32_t ref;
  JointType type;
  dJointID id;
};

static void defaultNearCallback(void* data, dGeomID a, dGeomID b) {
  lovrWorldCollide((World*) data, dGeomGetData(a), dGeomGetData(b), -1, -1);
}

static void customNearCallback(void* data, dGeomID shapeA, dGeomID shapeB) {
  World* world = data;
  arr_push(&world->overlaps, dGeomGetData(shapeA));
  arr_push(&world->overlaps, dGeomGetData(shapeB));
}

typedef struct {
  RaycastCallback callback;
  void* userdata;
  bool shouldStop;
} RaycastData;

static void raycastCallback(void* d, dGeomID a, dGeomID b) {
  RaycastData* data = d;
  if (data->shouldStop) return;
  RaycastCallback callback = data->callback;
  void* userdata = data->userdata;
  Shape* shape = dGeomGetData(b);
  Collider* collider = dBodyGetData(dGeomGetBody(b));

  if (!shape || !collider) {
    return;
  }

  dContact contact[MAX_CONTACTS];
  int count = dCollide(a, b, MAX_CONTACTS, &contact->geom, sizeof(dContact));
  for (int i = 0; i < count; i++) {
    dContactGeom g = contact[i].geom;
    data->shouldStop = callback(collider, 0, g.pos, g.normal, userdata);
  }
}

typedef struct {
  QueryCallback callback;
  void* userdata;
  bool called;
  bool shouldStop;
} QueryData;

static void queryCallback(void* d, dGeomID a, dGeomID b) {
  QueryData* data = d;
  if (data->shouldStop) return;

  Shape* shape = dGeomGetData(b);
  Collider* collider = dBodyGetData(dGeomGetBody(b));
  if (!shape || !collider) {
    return;
  }

  dContactGeom contact;
  if (dCollide(a, b, 1 | CONTACTS_UNIMPORTANT, &contact, sizeof(contact))) {
    if (data->callback) {
      data->shouldStop = data->callback(collider, 0, data->userdata);
    } else {
      data->shouldStop = true;
    }
    data->called = true;
  }
}

// XXX slow, but probably fine (tag names are not on any critical path), could switch to hashing if needed
static uint32_t findTag(World* world, const char* name) {
  if (name) {
    for (uint32_t i = 0; i < MAX_TAGS && world->tags[i]; i++) {
      if (!strcmp(world->tags[i], name)) {
        return i;
      }
    }
  }
  return NO_TAG;
}

static void onErrorMessage(int num, const char* format, va_list args) {
  char message[1024];
  vsnprintf(message, 1024, format, args);
  lovrLog(LOG_ERROR, "PHY", message);
}

static void onDebugMessage(int num, const char* format, va_list args) {
  char message[1024];
  vsnprintf(message, 1024, format, args);
  lovrLog(LOG_DEBUG, "PHY", message);
}

static void onInfoMessage(int num, const char* format, va_list args) {
  char message[1024];
  vsnprintf(message, 1024, format, args);
  lovrLog(LOG_INFO, "PHY", message);
}

static uint32_t ref;

bool lovrPhysicsInit(void) {
  if (atomic_fetch_add(&ref, 1)) return false;
  dInitODE();
  dSetErrorHandler(onErrorMessage);
  dSetDebugHandler(onDebugMessage);
  dSetMessageHandler(onInfoMessage);
  return true;
}

void lovrPhysicsDestroy(void) {
  if (atomic_fetch_sub(&ref, 1) != 1) return;
  dCloseODE();
}

World* lovrWorldCreate(WorldInfo* info) {
  World* world = lovrCalloc(sizeof(World));
  world->ref = 1;
  world->id = dWorldCreate();
  world->space = dHashSpaceCreate(0);
  dHashSpaceSetLevels(world->space, -4, 8);
  world->contactGroup = dJointGroupCreate(0);
  arr_init(&world->overlaps);
  lovrWorldSetGravity(world, info->gravity);
  lovrWorldSetSleepingAllowed(world, info->allowSleep);
  for (uint32_t i = 0; i < info->tagCount; i++) {
    size_t size = strlen(info->tags[i]) + 1;
    world->tags[i] = lovrMalloc(size);
    memcpy(world->tags[i], info->tags[i], size);
  }
  memset(world->masks, 0xff, sizeof(world->masks));
  return world;
}

void lovrWorldDestroy(void* ref) {
  World* world = ref;
  lovrWorldDestroyData(world);
  arr_free(&world->overlaps);
  for (uint32_t i = 0; i < MAX_TAGS && world->tags[i]; i++) {
    lovrFree(world->tags[i]);
  }
  lovrFree(world);
}

void lovrWorldDestroyData(World* world) {
  while (world->head) {
    Collider* next = world->head->next;
    lovrColliderDestroyData(world->head);
    world->head = next;
  }

  if (world->contactGroup) {
    dJointGroupDestroy(world->contactGroup);
    world->contactGroup = NULL;
  }

  if (world->space) {
    dSpaceDestroy(world->space);
    world->space = NULL;
  }

  if (world->id) {
    dWorldDestroy(world->id);
    world->id = NULL;
  }
}

void lovrWorldUpdate(World* world, float dt, CollisionResolver resolver, void* userdata) {
  if (resolver) {
    resolver(world, userdata);
  } else {
    dSpaceCollide(world->space, world, defaultNearCallback);
  }

  if (dt > 0) {
    dWorldQuickStep(world->id, dt);
  }

  dJointGroupEmpty(world->contactGroup);
}

int lovrWorldGetStepCount(World* world) {
  return dWorldGetQuickStepNumIterations(world->id);
}

void lovrWorldSetStepCount(World* world, int iterations) {
  dWorldSetQuickStepNumIterations(world->id, iterations);
}

void lovrWorldComputeOverlaps(World* world) {
  arr_clear(&world->overlaps);
  dSpaceCollide(world->space, world, customNearCallback);
}

int lovrWorldGetNextOverlap(World* world, Shape** a, Shape** b) {
  if (world->overlaps.length == 0) {
    *a = *b = NULL;
    return 0;
  }

  *a = arr_pop(&world->overlaps);
  *b = arr_pop(&world->overlaps);
  return 1;
}

int lovrWorldCollide(World* world, Shape* a, Shape* b, float friction, float restitution) {
  if (!a || !b) {
    return false;
  }

  Collider* colliderA = a->collider;
  Collider* colliderB = b->collider;
  uint32_t i = colliderA->tag;
  uint32_t j = colliderB->tag;

  if (i != NO_TAG && j != NO_TAG && !((world->masks[i] & (1 << j)) && (world->masks[j] & (1 << i)))) {
    return false;
  }

  if (friction < 0.f) {
    friction = sqrtf(colliderA->friction * colliderB->friction);
  }

  if (restitution < 0.f) {
    restitution = MAX(colliderA->restitution, colliderB->restitution);
  }

  dContact contacts[MAX_CONTACTS];
  for (int c = 0; c < MAX_CONTACTS; c++) {
    contacts[c].surface.mode = 0;
    contacts[c].surface.mu = friction;
    contacts[c].surface.bounce = restitution;

    if (restitution > 0) {
      contacts[c].surface.mode |= dContactBounce;
    }
  }

  int contactCount = dCollide(a->id, b->id, MAX_CONTACTS, &contacts[0].geom, sizeof(dContact));

  if (!a->sensor && !b->sensor) {
    for (int c = 0; c < contactCount; c++) {
      dJointID joint = dJointCreateContact(world->id, world->contactGroup, &contacts[c]);
      dJointAttach(joint, colliderA->body, colliderB->body);
    }
  }

  return contactCount;
}

void lovrWorldGetContacts(World* world, Shape* a, Shape* b, Contact contacts[MAX_CONTACTS], uint32_t* count) {
  dContactGeom info[MAX_CONTACTS];
  int c = *count = dCollide(a->id, b->id, MAX_CONTACTS, info, sizeof(info[0]));
  for (int i = 0; i < c; i++) {
    contacts[i] = (Contact) {
      .x = info[i].pos[0],
      .y = info[i].pos[1],
      .z = info[i].pos[2],
      .nx = info[i].normal[0],
      .ny = info[i].normal[1],
      .nz = info[i].normal[2],
      .depth = info[i].depth
    };
  }
}

void lovrWorldRaycast(World* world, float x1, float y1, float z1, float x2, float y2, float z2, RaycastCallback callback, void* userdata) {
  RaycastData data = { .callback = callback, .userdata = userdata, .shouldStop = false };
  float dx = x2 - x1;
  float dy = y2 - y1;
  float dz = z2 - z1;
  float length = sqrtf(dx * dx + dy * dy + dz * dz);
  dGeomID ray = dCreateRay(world->space, length);
  dGeomRaySet(ray, x1, y1, z1, dx, dy, dz);
  dSpaceCollide2(ray, (dGeomID) world->space, &data, raycastCallback);
  dGeomDestroy(ray);
}

bool lovrWorldQueryBox(World* world, float position[3], float size[3], QueryCallback callback, void* userdata) {
  QueryData data = { .callback = callback, .userdata = userdata, .called = false, .shouldStop = false };
  dGeomID box = dCreateBox(world->space, fabsf(size[0]), fabsf(size[1]), fabsf(size[2]));
  dGeomSetPosition(box, position[0], position[1], position[2]);
  dSpaceCollide2(box, (dGeomID) world->space, &data, queryCallback);
  dGeomDestroy(box);
  return data.called;
}

bool lovrWorldQuerySphere(World* world, float position[3], float radius, QueryCallback callback, void* userdata) {
  QueryData data = { .callback = callback, .userdata = userdata, .called = false, .shouldStop = false };
  dGeomID sphere = dCreateSphere(world->space, fabsf(radius));
  dGeomSetPosition(sphere, position[0], position[1], position[2]);
  dSpaceCollide2(sphere, (dGeomID) world->space, &data, queryCallback);
  dGeomDestroy(sphere);
  return data.called;
}

Collider* lovrWorldGetFirstCollider(World* world) {
  return world->head;
}

void lovrWorldGetGravity(World* world, float gravity[3]) {
  dReal g[4];
  dWorldGetGravity(world->id, g);
  gravity[0] = g[0];
  gravity[1] = g[1];
  gravity[2] = g[2];
}

void lovrWorldSetGravity(World* world, float gravity[3]) {
  dWorldSetGravity(world->id, gravity[0], gravity[1], gravity[2]);
}

float lovrWorldGetResponseTime(World* world) {
  return dWorldGetCFM(world->id);
}

void lovrWorldSetResponseTime(World* world, float responseTime) {
  dWorldSetCFM(world->id, responseTime);
}

float lovrWorldGetTightness(World* world) {
  return dWorldGetERP(world->id);
}

void lovrWorldSetTightness(World* world, float tightness) {
  dWorldSetERP(world->id, tightness);
}

void lovrWorldGetLinearDamping(World* world, float* damping, float* threshold) {
  *damping = dWorldGetLinearDamping(world->id);
  *threshold = dWorldGetLinearDampingThreshold(world->id);
}

void lovrWorldSetLinearDamping(World* world, float damping, float threshold) {
  dWorldSetLinearDamping(world->id, damping);
  dWorldSetLinearDampingThreshold(world->id, threshold);
}

void lovrWorldGetAngularDamping(World* world, float* damping, float* threshold) {
  *damping = dWorldGetAngularDamping(world->id);
  *threshold = dWorldGetAngularDampingThreshold(world->id);
}

void lovrWorldSetAngularDamping(World* world, float damping, float threshold) {
  dWorldSetAngularDamping(world->id, damping);
  dWorldSetAngularDampingThreshold(world->id, threshold);
}

bool lovrWorldIsSleepingAllowed(World* world) {
  return dWorldGetAutoDisableFlag(world->id);
}

void lovrWorldSetSleepingAllowed(World* world, bool allowed) {
  dWorldSetAutoDisableFlag(world->id, allowed);
}

const char* lovrWorldGetTagName(World* world, uint32_t tag) {
  return (tag == NO_TAG) ? NULL : world->tags[tag];
}

void lovrWorldDisableCollisionBetween(World* world, const char* tag1, const char* tag2) {
  uint32_t i = findTag(world, tag1);
  uint32_t j = findTag(world, tag2);

  if (i == NO_TAG || j == NO_TAG) {
    return;
  }

  world->masks[i] &= ~(1 << j);
  world->masks[j] &= ~(1 << i);
  return;
}

void lovrWorldEnableCollisionBetween(World* world, const char* tag1, const char* tag2) {
  uint32_t i = findTag(world, tag1);
  uint32_t j = findTag(world, tag2);

  if (i == NO_TAG || j == NO_TAG) {
    return;
  }

  world->masks[i] |= (1 << j);
  world->masks[j] |= (1 << i);
  return;
}

bool lovrWorldIsCollisionEnabledBetween(World* world, const char* tag1, const char* tag2) {
  uint32_t i = findTag(world, tag1);
  uint32_t j = findTag(world, tag2);

  if (i == NO_TAG || j == NO_TAG) {
    return true;
  }

  return (world->masks[i] & (1 << j)) && (world->masks[j] & (1 << i));
}

Collider* lovrColliderCreate(World* world, Shape* shape, float x, float y, float z) {
  Collider* collider = lovrCalloc(sizeof(Collider));
  collider->ref = 1;
  collider->body = dBodyCreate(world->id);
  collider->world = world;
  collider->friction = INFINITY;
  collider->restitution = 0;
  collider->tag = NO_TAG;
  dBodySetData(collider->body, collider);
  arr_init(&collider->joints);

  lovrColliderSetShape(collider, shape);
  lovrColliderSetPosition(collider, x, y, z);

  // Adjust the world's collider list
  if (!collider->world->head) {
    collider->world->head = collider;
  } else {
    collider->next = collider->world->head;
    collider->next->prev = collider;
    collider->world->head = collider;
  }

  // The world owns a reference to the collider
  lovrRetain(collider);
  return collider;
}

void lovrColliderDestroy(void* ref) {
  Collider* collider = ref;
  lovrColliderDestroyData(collider);
  arr_free(&collider->joints);
  lovrFree(collider);
}

void lovrColliderDestroyData(Collider* collider) {
  if (!collider->body) {
    return;
  }

  lovrColliderSetShape(collider, NULL);

  size_t count;
  Joint** joints = lovrColliderGetJoints(collider, &count);
  for (size_t i = 0; i < count; i++) {
    lovrRelease(joints[i], lovrJointDestroy);
  }

  dBodyDestroy(collider->body);
  collider->body = NULL;

  if (collider->next) collider->next->prev = collider->prev;
  if (collider->prev) collider->prev->next = collider->next;
  if (collider->world->head == collider) collider->world->head = collider->next;
  collider->next = collider->prev = NULL;

  // If the Collider is destroyed, the world lets go of its reference to this Collider
  lovrRelease(collider, lovrColliderDestroy);
}

bool lovrColliderIsDestroyed(Collider* collider) {
  return !collider->body;
}

void lovrColliderInitInertia(Collider* collider, Shape* shape) {
  // compute inertia matrix for default density
  const float density = 1.0f;
  float cx, cy, cz, mass, inertia[6];
  lovrShapeGetMass(shape, density, &cx, &cy, &cz, &mass, inertia);
  lovrColliderSetMassData(collider, cx, cy, cz, mass, inertia);
}

World* lovrColliderGetWorld(Collider* collider) {
  return collider->world;
}

Collider* lovrColliderGetNext(Collider* collider) {
  return collider->next;
}

Shape* lovrColliderGetShape(Collider* collider) {
  return collider->shape;
}

void lovrColliderSetShape(Collider* collider, Shape* shape) {
  if (collider->shape) {
    dSpaceRemove(collider->world->space, collider->shape->id);
    dGeomSetBody(collider->shape->id, 0);
    collider->shape->collider = NULL;
    lovrRelease(collider->shape, lovrShapeDestroy);
  }

  collider->shape = shape;

  if (shape) {
    if (shape->collider) {
      lovrColliderSetShape(shape->collider, NULL);
    }

    shape->collider = collider;
    dGeomSetBody(shape->id, collider->body);
    dSpaceID newSpace = collider->world->space;
    dSpaceAdd(newSpace, shape->id);
    lovrRetain(shape);
  }
}

void lovrColliderGetShapeOffset(Collider* collider, float* position, float* orientation) {
  const dReal* p = dGeomGetOffsetPosition(collider->shape->id);
  position[0] = p[0];
  position[1] = p[1];
  position[2] = p[2];
  dReal q[4];
  dGeomGetOffsetQuaternion(collider->shape->id, q);
  orientation[0] = q[1];
  orientation[1] = q[2];
  orientation[2] = q[3];
  orientation[3] = q[0];
}

void lovrColliderSetShapeOffset(Collider* collider, float* position, float* orientation) {
  dGeomSetOffsetPosition(collider->shape->id, position[0], position[1], position[2]);
  dReal q[4] = { orientation[3], orientation[0], orientation[1], orientation[2] };
  dGeomSetOffsetQuaternion(collider->shape->id, q);
}

Joint** lovrColliderGetJoints(Collider* collider, size_t* count) {
  arr_clear(&collider->joints);
  int jointCount = dBodyGetNumJoints(collider->body);
  for (int i = 0; i < jointCount; i++) {
    Joint* joint = dJointGetData(dBodyGetJoint(collider->body, i));
    if (joint) {
      arr_push(&collider->joints, joint);
    }
  }
  *count = collider->joints.length;
  return collider->joints.data;
}

const char* lovrColliderGetTag(Collider* collider) {
  return lovrWorldGetTagName(collider->world, collider->tag);
}

bool lovrColliderSetTag(Collider* collider, const char* tag) {
  if (!tag) {
    collider->tag = NO_TAG;
    return true;
  }

  collider->tag = findTag(collider->world, tag);
  return collider->tag != NO_TAG;
}

float lovrColliderGetFriction(Collider* collider) {
  return collider->friction;
}

void lovrColliderSetFriction(Collider* collider, float friction) {
  collider->friction = friction;
}

float lovrColliderGetRestitution(Collider* collider) {
  return collider->restitution;
}

void lovrColliderSetRestitution(Collider* collider, float restitution) {
  collider->restitution = restitution;
}

bool lovrColliderIsKinematic(Collider* collider) {
  return dBodyIsKinematic(collider->body);
}

void lovrColliderSetKinematic(Collider* collider, bool kinematic) {
  if (kinematic) {
    dBodySetKinematic(collider->body);
  } else {
    dBodySetDynamic(collider->body);
  }
}

bool lovrColliderIsContinuous(Collider* collider) {
  return false;
}

void lovrColliderSetContinuous(Collider* collider, bool continuous) {
  //
}

float lovrColliderGetGravityScale(Collider* collider) {
  return dBodyGetGravityMode(collider->body) ? 1.f : 0.f;
}

void lovrColliderSetGravityScale(Collider* collider, float scale) {
  dBodySetGravityMode(collider->body, scale == 0.f ? false : true);
}

bool lovrColliderIsSleepingAllowed(Collider* collider) {
  return dBodyGetAutoDisableFlag(collider->body);
}

void lovrColliderSetSleepingAllowed(Collider* collider, bool allowed) {
  dBodySetAutoDisableFlag(collider->body, allowed);
}

bool lovrColliderIsAwake(Collider* collider) {
  return dBodyIsEnabled(collider->body);
}

void lovrColliderSetAwake(Collider* collider, bool awake) {
  if (awake) {
    dBodyEnable(collider->body);
  } else {
    dBodyDisable(collider->body);
  }
}

float lovrColliderGetMass(Collider* collider) {
  dMass m;
  dBodyGetMass(collider->body, &m);
  return m.mass;
}

void lovrColliderSetMass(Collider* collider, float mass) {
  dMass m;
  dBodyGetMass(collider->body, &m);
  dMassAdjust(&m, mass);
  dBodySetMass(collider->body, &m);
}

void lovrColliderGetMassData(Collider* collider, float* cx, float* cy, float* cz, float* mass, float inertia[6]) {
  dMass m;
  dBodyGetMass(collider->body, &m);
  *cx = m.c[0];
  *cy = m.c[1];
  *cz = m.c[2];
  *mass = m.mass;

  // Diagonal
  inertia[0] = m.I[0];
  inertia[1] = m.I[5];
  inertia[2] = m.I[10];

  // Lower triangular
  inertia[3] = m.I[4];
  inertia[4] = m.I[8];
  inertia[5] = m.I[9];
}

void lovrColliderSetMassData(Collider* collider, float cx, float cy, float cz, float mass, float inertia[6]) {
  dMass m;
  dBodyGetMass(collider->body, &m);
  dMassSetParameters(&m, mass, cx, cy, cz, inertia[0], inertia[1], inertia[2], inertia[3], inertia[4], inertia[5]);
  dBodySetMass(collider->body, &m);
}

void lovrColliderGetPosition(Collider* collider, float* x, float* y, float* z) {
  const dReal* position = dBodyGetPosition(collider->body);
  *x = position[0];
  *y = position[1];
  *z = position[2];
}

void lovrColliderSetPosition(Collider* collider, float x, float y, float z) {
  dBodySetPosition(collider->body, x, y, z);
}

void lovrColliderGetOrientation(Collider* collider, float* orientation) {
  const dReal* q = dBodyGetQuaternion(collider->body);
  orientation[0] = q[1];
  orientation[1] = q[2];
  orientation[2] = q[3];
  orientation[3] = q[0];
}

void lovrColliderSetOrientation(Collider* collider, float* orientation) {
  dReal q[4] = { orientation[3], orientation[0], orientation[1], orientation[2] };
  dBodySetQuaternion(collider->body, q);
}

void lovrColliderGetLinearVelocity(Collider* collider, float* x, float* y, float* z) {
  const dReal* velocity = dBodyGetLinearVel(collider->body);
  *x = velocity[0];
  *y = velocity[1];
  *z = velocity[2];
}

void lovrColliderSetLinearVelocity(Collider* collider, float x, float y, float z) {
  dBodyEnable(collider->body);
  dBodySetLinearVel(collider->body, x, y, z);
}

void lovrColliderGetAngularVelocity(Collider* collider, float* x, float* y, float* z) {
  const dReal* velocity = dBodyGetAngularVel(collider->body);
  *x = velocity[0];
  *y = velocity[1];
  *z = velocity[2];
}

void lovrColliderSetAngularVelocity(Collider* collider, float x, float y, float z) {
  dBodyEnable(collider->body);
  dBodySetAngularVel(collider->body, x, y, z);
}

void lovrColliderGetLinearDamping(Collider* collider, float* damping, float* threshold) {
  *damping = dBodyGetLinearDamping(collider->body);
  *threshold = dBodyGetLinearDampingThreshold(collider->body);
}

void lovrColliderSetLinearDamping(Collider* collider, float damping, float threshold) {
  dBodySetLinearDamping(collider->body, damping);
  dBodySetLinearDampingThreshold(collider->body, threshold);
}

void lovrColliderGetAngularDamping(Collider* collider, float* damping, float* threshold) {
  *damping = dBodyGetAngularDamping(collider->body);
  *threshold = dBodyGetAngularDampingThreshold(collider->body);
}

void lovrColliderSetAngularDamping(Collider* collider, float damping, float threshold) {
  dBodySetAngularDamping(collider->body, damping);
  dBodySetAngularDampingThreshold(collider->body, threshold);
}

void lovrColliderApplyForce(Collider* collider, float x, float y, float z) {
  dBodyEnable(collider->body);
  dBodyAddForce(collider->body, x, y, z);
}

void lovrColliderApplyForceAtPosition(Collider* collider, float x, float y, float z, float cx, float cy, float cz) {
  dBodyEnable(collider->body);
  dBodyAddForceAtPos(collider->body, x, y, z, cx, cy, cz);
}

void lovrColliderApplyTorque(Collider* collider, float x, float y, float z) {
  dBodyEnable(collider->body);
  dBodyAddTorque(collider->body, x, y, z);
}

void lovrColliderApplyLinearImpulse(Collider* collider, float impulse[3]) {
  //
}

void lovrColliderApplyLinearImpulseAtPosition(Collider* collider, float impulse[3], float position[3]) {
  //
}

void lovrColliderApplyAngularImpulse(Collider* collider, float impulse[3]) {
  //
}

void lovrColliderGetLocalCenter(Collider* collider, float* x, float* y, float* z) {
  dMass m;
  dBodyGetMass(collider->body, &m);
  *x = m.c[0];
  *y = m.c[1];
  *z = m.c[2];
}

void lovrColliderGetLocalPoint(Collider* collider, float wx, float wy, float wz, float* x, float* y, float* z) {
  dReal local[4];
  dBodyGetPosRelPoint(collider->body, wx, wy, wz, local);
  *x = local[0];
  *y = local[1];
  *z = local[2];
}

void lovrColliderGetWorldPoint(Collider* collider, float x, float y, float z, float* wx, float* wy, float* wz) {
  dReal world[4];
  dBodyGetRelPointPos(collider->body, x, y, z, world);
  *wx = world[0];
  *wy = world[1];
  *wz = world[2];
}

void lovrColliderGetLocalVector(Collider* collider, float wx, float wy, float wz, float* x, float* y, float* z) {
  dReal local[4];
  dBodyVectorFromWorld(collider->body, wx, wy, wz, local);
  *x = local[0];
  *y = local[1];
  *z = local[2];
}

void lovrColliderGetWorldVector(Collider* collider, float x, float y, float z, float* wx, float* wy, float* wz) {
  dReal world[4];
  dBodyVectorToWorld(collider->body, x, y, z, world);
  *wx = world[0];
  *wy = world[1];
  *wz = world[2];
}

void lovrColliderGetLinearVelocityFromLocalPoint(Collider* collider, float x, float y, float z, float* vx, float* vy, float* vz) {
  dReal velocity[4];
  dBodyGetRelPointVel(collider->body, x, y, z, velocity);
  *vx = velocity[0];
  *vy = velocity[1];
  *vz = velocity[2];
}

void lovrColliderGetLinearVelocityFromWorldPoint(Collider* collider, float wx, float wy, float wz, float* vx, float* vy, float* vz) {
  dReal velocity[4];
  dBodyGetPointVel(collider->body, wx, wy, wz, velocity);
  *vx = velocity[0];
  *vy = velocity[1];
  *vz = velocity[2];
}

void lovrColliderGetAABB(Collider* collider, float aabb[6]) {
  dGeomID shape = dBodyGetFirstGeom(collider->body);

  if (!shape) {
    memset(aabb, 0, 6 * sizeof(float));
    return;
  }

  dGeomGetAABB(shape, aabb);

  float otherAABB[6];
  while ((shape = dBodyGetNextGeom(shape)) != NULL) {
    dGeomGetAABB(shape, otherAABB);
    aabb[0] = MIN(aabb[0], otherAABB[0]);
    aabb[1] = MAX(aabb[1], otherAABB[1]);
    aabb[2] = MIN(aabb[2], otherAABB[2]);
    aabb[3] = MAX(aabb[3], otherAABB[3]);
    aabb[4] = MIN(aabb[4], otherAABB[4]);
    aabb[5] = MAX(aabb[5], otherAABB[5]);
  }
}

void lovrShapeDestroy(void* ref) {
  Shape* shape = ref;
  lovrShapeDestroyData(shape);
  lovrFree(shape);
}

void lovrShapeDestroyData(Shape* shape) {
  if (shape->id) {
    if (shape->type == SHAPE_MESH) {
      dTriMeshDataID dataID = dGeomTriMeshGetData(shape->id);
      dGeomTriMeshDataDestroy(dataID);
      lovrFree(shape->vertices);
      lovrFree(shape->indices);
    } else if (shape->type == SHAPE_TERRAIN) {
      dHeightfieldDataID dataID = dGeomHeightfieldGetHeightfieldData(shape->id);
      dGeomHeightfieldDataDestroy(dataID);
    }
    dGeomDestroy(shape->id);
    shape->id = NULL;
  }
}

ShapeType lovrShapeGetType(Shape* shape) {
  return shape->type;
}

Collider* lovrShapeGetCollider(Shape* shape) {
  return shape->collider;
}

bool lovrShapeIsEnabled(Shape* shape) {
  return dGeomIsEnabled(shape->id);
}

void lovrShapeSetEnabled(Shape* shape, bool enabled) {
  if (enabled) {
    dGeomEnable(shape->id);
  } else {
    dGeomDisable(shape->id);
  }
}

bool lovrShapeIsSensor(Shape* shape) {
  return shape->sensor;
}

void lovrShapeSetSensor(Shape* shape, bool sensor) {
  shape->sensor = sensor;
}

void lovrShapeGetMass(Shape* shape, float density, float* cx, float* cy, float* cz, float* mass, float inertia[6]) {
  dMass m;
  dMassSetZero(&m);
  switch (shape->type) {
    case SHAPE_SPHERE: {
      dMassSetSphere(&m, density, dGeomSphereGetRadius(shape->id));
      break;
    }

    case SHAPE_BOX: {
      dReal lengths[4];
      dGeomBoxGetLengths(shape->id, lengths);
      dMassSetBox(&m, density, lengths[0], lengths[1], lengths[2]);
      break;
    }

    case SHAPE_CAPSULE: {
      dReal radius, length;
      dGeomCapsuleGetParams(shape->id, &radius, &length);
      dMassSetCapsule(&m, density, 3, radius, length);
      break;
    }

    case SHAPE_CYLINDER: {
      dReal radius, length;
      dGeomCylinderGetParams(shape->id, &radius, &length);
      dMassSetCylinder(&m, density, 3, radius, length);
      break;
    }

    case SHAPE_MESH: {
      dMassSetTrimesh(&m, density, shape->id);
      dGeomSetPosition(shape->id, -m.c[0], -m.c[1], -m.c[2]);
      dMassTranslate(&m, -m.c[0], -m.c[1], -m.c[2]);
      break;
    }

    case SHAPE_TERRAIN: {
      break;
    }

    default: break;
  }

  const dReal* position = dGeomGetOffsetPosition(shape->id);
  dMassTranslate(&m, position[0], position[1], position[2]);
  const dReal* rotation = dGeomGetOffsetRotation(shape->id);
  dMassRotate(&m, rotation);

  *cx = m.c[0];
  *cy = m.c[1];
  *cz = m.c[2];
  *mass = m.mass;

  // Diagonal
  inertia[0] = m.I[0];
  inertia[1] = m.I[5];
  inertia[2] = m.I[10];

  // Lower triangular
  inertia[3] = m.I[4];
  inertia[4] = m.I[8];
  inertia[5] = m.I[9];
}

void lovrShapeGetAABB(Shape* shape, float aabb[6]) {
  dGeomGetAABB(shape->id, aabb);
}

SphereShape* lovrSphereShapeCreate(float radius) {
  lovrCheck(radius > 0.f, "SphereShape radius must be positive");
  SphereShape* sphere = lovrCalloc(sizeof(SphereShape));
  sphere->ref = 1;
  sphere->type = SHAPE_SPHERE;
  sphere->id = dCreateSphere(0, radius);
  dGeomSetData(sphere->id, sphere);
  return sphere;
}

float lovrSphereShapeGetRadius(SphereShape* sphere) {
  return dGeomSphereGetRadius(sphere->id);
}

void lovrSphereShapeSetRadius(SphereShape* sphere, float radius) {
  lovrCheck(radius > 0.f, "SphereShape radius must be positive");
  dGeomSphereSetRadius(sphere->id, radius);
}

BoxShape* lovrBoxShapeCreate(float w, float h, float d) {
  BoxShape* box = lovrCalloc(sizeof(BoxShape));
  box->ref = 1;
  box->type = SHAPE_BOX;
  box->id = dCreateBox(0, w, h, d);
  dGeomSetData(box->id, box);
  return box;
}

void lovrBoxShapeGetDimensions(BoxShape* box, float* w, float* h, float* d) {
  dReal dimensions[4];
  dGeomBoxGetLengths(box->id, dimensions);
  *w = dimensions[0];
  *h = dimensions[1];
  *d = dimensions[2];
}

void lovrBoxShapeSetDimensions(BoxShape* box, float w, float h, float d) {
  lovrCheck(w > 0.f && h > 0.f && d > 0.f, "BoxShape dimensions must be positive");
  dGeomBoxSetLengths(box->id, w, h, d);
}

CapsuleShape* lovrCapsuleShapeCreate(float radius, float length) {
  lovrCheck(radius > 0.f && length > 0.f, "CapsuleShape dimensions must be positive");
  CapsuleShape* capsule = lovrCalloc(sizeof(CapsuleShape));
  capsule->ref = 1;
  capsule->type = SHAPE_CAPSULE;
  capsule->id = dCreateCapsule(0, radius, length);
  dGeomSetData(capsule->id, capsule);
  return capsule;
}

float lovrCapsuleShapeGetRadius(CapsuleShape* capsule) {
  dReal radius, length;
  dGeomCapsuleGetParams(capsule->id, &radius, &length);
  return radius;
}

void lovrCapsuleShapeSetRadius(CapsuleShape* capsule, float radius) {
  lovrCheck(radius > 0.f, "CapsuleShape dimensions must be positive");
  dGeomCapsuleSetParams(capsule->id, radius, lovrCapsuleShapeGetLength(capsule));
}

float lovrCapsuleShapeGetLength(CapsuleShape* capsule) {
  dReal radius, length;
  dGeomCapsuleGetParams(capsule->id, &radius, &length);
  return length;
}

void lovrCapsuleShapeSetLength(CapsuleShape* capsule, float length) {
  lovrCheck(length > 0.f, "CapsuleShape dimensions must be positive");
  dGeomCapsuleSetParams(capsule->id, lovrCapsuleShapeGetRadius(capsule), length);
}

CylinderShape* lovrCylinderShapeCreate(float radius, float length) {
  lovrCheck(radius > 0.f && length > 0.f, "CylinderShape dimensions must be positive");
  CylinderShape* cylinder = lovrCalloc(sizeof(CylinderShape));
  cylinder->ref = 1;
  cylinder->type = SHAPE_CYLINDER;
  cylinder->id = dCreateCylinder(0, radius, length);
  dGeomSetData(cylinder->id, cylinder);
  return cylinder;
}

float lovrCylinderShapeGetRadius(CylinderShape* cylinder) {
  dReal radius, length;
  dGeomCylinderGetParams(cylinder->id, &radius, &length);
  return radius;
}

void lovrCylinderShapeSetRadius(CylinderShape* cylinder, float radius) {
  lovrCheck(radius > 0.f, "CylinderShape dimensions must be positive");
  dGeomCylinderSetParams(cylinder->id, radius, lovrCylinderShapeGetLength(cylinder));
}

float lovrCylinderShapeGetLength(CylinderShape* cylinder) {
  dReal radius, length;
  dGeomCylinderGetParams(cylinder->id, &radius, &length);
  return length;
}

void lovrCylinderShapeSetLength(CylinderShape* cylinder, float length) {
  lovrCheck(length > 0.f, "CylinderShape dimensions must be positive");
  dGeomCylinderSetParams(cylinder->id, lovrCylinderShapeGetRadius(cylinder), length);
}

MeshShape* lovrMeshShapeCreate(int vertexCount, float* vertices, int indexCount, dTriIndex* indices) {
  MeshShape* mesh = lovrCalloc(sizeof(MeshShape));
  mesh->ref = 1;
  dTriMeshDataID dataID = dGeomTriMeshDataCreate();
  dGeomTriMeshDataBuildSingle(dataID, vertices, 3 * sizeof(float), vertexCount, indices, indexCount, 3 * sizeof(dTriIndex));
  dGeomTriMeshDataPreprocess2(dataID, (1U << dTRIDATAPREPROCESS_BUILD_FACE_ANGLES), NULL);
  mesh->id = dCreateTriMesh(0, dataID, 0, 0, 0);
  mesh->type = SHAPE_MESH;
  mesh->vertices = vertices;
  mesh->indices = indices;
  dGeomSetData(mesh->id, mesh);
  return mesh;
}

TerrainShape* lovrTerrainShapeCreate(float* vertices, uint32_t n, float scaleXZ, float scaleY) {
  const float thickness = 10.f;
  TerrainShape* terrain = lovrCalloc(sizeof(TerrainShape));
  terrain->ref = 1;
  dHeightfieldDataID dataID = dGeomHeightfieldDataCreate();
  dGeomHeightfieldDataBuildSingle(dataID, vertices, 1, scaleXZ, scaleXZ, n, n, scaleY, 0.f, thickness, 0);
  terrain->id = dCreateHeightfield(0, dataID, 1);
  terrain->type = SHAPE_TERRAIN;
  dGeomSetData(terrain->id, terrain);
  return terrain;
}

CompoundShape* lovrCompoundShapeCreate(Shape** shapes, float* positions, float* orientations, uint32_t count, bool freeze) {
  lovrThrow("ODE does not support compound shape");
}

bool lovrCompoundShapeIsFrozen(CompoundShape* shape) {
  return false;
}

void lovrCompoundShapeAddShape(CompoundShape* shape, Shape* child, float* position, float* orientation) {
  //
}

void lovrCompoundShapeReplaceShape(CompoundShape* shape, uint32_t index, Shape* child, float* position, float* orientation) {
  //
}

void lovrCompoundShapeRemoveShape(CompoundShape* shape, uint32_t index) {
  //
}

Shape* lovrCompoundShapeGetShape(CompoundShape* shape, uint32_t index) {
  return NULL;
}

uint32_t lovrCompoundShapeGetShapeCount(CompoundShape* shape) {
  return 0;
}

void lovrCompoundShapeGetShapeOffset(CompoundShape* shape, uint32_t index, float* position, float* orientation) {
  //
}

void lovrCompoundShapeSetShapeOffset(CompoundShape* shape, uint32_t index, float* position, float* orientation) {
  //
}

void lovrJointDestroy(void* ref) {
  Joint* joint = ref;
  lovrJointDestroyData(joint);
  lovrFree(joint);
}

void lovrJointDestroyData(Joint* joint) {
  if (joint->id) {
    dJointDestroy(joint->id);
    joint->id = NULL;
  }
}

JointType lovrJointGetType(Joint* joint) {
  return joint->type;
}

void lovrJointGetColliders(Joint* joint, Collider** a, Collider** b) {
  dBodyID bodyA = dJointGetBody(joint->id, 0);
  dBodyID bodyB = dJointGetBody(joint->id, 1);

  if (bodyA) {
    *a = dBodyGetData(bodyA);
  }

  if (bodyB) {
    *b = dBodyGetData(bodyB);
  }
}

bool lovrJointIsEnabled(Joint* joint) {
  return dJointIsEnabled(joint->id);
}

void lovrJointSetEnabled(Joint* joint, bool enable) {
  if (enable) {
    dJointEnable(joint->id);
  } else {
    dJointDisable(joint->id);
  }
}

BallJoint* lovrBallJointCreate(Collider* a, Collider* b, float anchor[3]) {
  lovrCheck(a->world == b->world, "Joint bodies must exist in same World");
  BallJoint* joint = lovrCalloc(sizeof(BallJoint));
  joint->ref = 1;
  joint->type = JOINT_BALL;
  joint->id = dJointCreateBall(a->world->id, 0);
  dJointSetData(joint->id, joint);
  dJointAttach(joint->id, a->body, b->body);
  lovrBallJointSetAnchor(joint, anchor);
  lovrRetain(joint);
  return joint;
}

void lovrBallJointGetAnchors(BallJoint* joint, float anchor1[3], float anchor2[3]) {
  dReal anchor[4];
  dJointGetBallAnchor(joint->id, anchor);
  anchor1[0] = anchor[0];
  anchor1[1] = anchor[1];
  anchor1[2] = anchor[2];
  dJointGetBallAnchor2(joint->id, anchor);
  anchor2[0] = anchor[0];
  anchor2[1] = anchor[1];
  anchor2[2] = anchor[2];
}

void lovrBallJointSetAnchor(BallJoint* joint, float anchor[3]) {
  dJointSetBallAnchor(joint->id, anchor[0], anchor[1], anchor[2]);
}

float lovrBallJointGetResponseTime(Joint* joint) {
  return dJointGetBallParam(joint->id, dParamCFM);
}

void lovrBallJointSetResponseTime(Joint* joint, float responseTime) {
  dJointSetBallParam(joint->id, dParamCFM, responseTime);
}

float lovrBallJointGetTightness(Joint* joint) {
  return dJointGetBallParam(joint->id, dParamERP);
}

void lovrBallJointSetTightness(Joint* joint, float tightness) {
  dJointSetBallParam(joint->id, dParamERP, tightness);
}

DistanceJoint* lovrDistanceJointCreate(Collider* a, Collider* b, float anchor1[3], float anchor2[3]) {
  lovrCheck(a->world == b->world, "Joint bodies must exist in same World");
  DistanceJoint* joint = lovrCalloc(sizeof(DistanceJoint));
  joint->ref = 1;
  joint->type = JOINT_DISTANCE;
  joint->id = dJointCreateDBall(a->world->id, 0);
  dJointSetData(joint->id, joint);
  dJointAttach(joint->id, a->body, b->body);
  lovrDistanceJointSetAnchors(joint, anchor1, anchor2);
  lovrRetain(joint);
  return joint;
}

void lovrDistanceJointGetAnchors(DistanceJoint* joint, float anchor1[3], float anchor2[3]) {
  dReal anchor[4];
  dJointGetDBallAnchor1(joint->id, anchor);
  anchor1[0] = anchor[0];
  anchor1[1] = anchor[1];
  anchor1[2] = anchor[2];
  dJointGetDBallAnchor2(joint->id, anchor);
  anchor2[0] = anchor[0];
  anchor2[1] = anchor[1];
  anchor2[2] = anchor[2];
}

void lovrDistanceJointSetAnchors(DistanceJoint* joint, float anchor1[3], float anchor2[3]) {
  dJointSetDBallAnchor1(joint->id, anchor1[0], anchor1[1], anchor1[2]);
  dJointSetDBallAnchor2(joint->id, anchor2[0], anchor2[1], anchor2[2]);
}

float lovrDistanceJointGetDistance(DistanceJoint* joint) {
  return dJointGetDBallDistance(joint->id);
}

void lovrDistanceJointSetDistance(DistanceJoint* joint, float distance) {
  dJointSetDBallDistance(joint->id, distance);
}

float lovrDistanceJointGetResponseTime(Joint* joint) {
  return dJointGetDBallParam(joint->id, dParamCFM);
}

void lovrDistanceJointSetResponseTime(Joint* joint, float responseTime) {
  dJointSetDBallParam(joint->id, dParamCFM, responseTime);
}

float lovrDistanceJointGetTightness(Joint* joint) {
  return dJointGetDBallParam(joint->id, dParamERP);
}

void lovrDistanceJointSetTightness(Joint* joint, float tightness) {
  dJointSetDBallParam(joint->id, dParamERP, tightness);
}

HingeJoint* lovrHingeJointCreate(Collider* a, Collider* b, float anchor[3], float axis[3]) {
  lovrCheck(a->world == b->world, "Joint bodies must exist in same World");
  HingeJoint* joint = lovrCalloc(sizeof(HingeJoint));
  joint->ref = 1;
  joint->type = JOINT_HINGE;
  joint->id = dJointCreateHinge(a->world->id, 0);
  dJointSetData(joint->id, joint);
  dJointAttach(joint->id, a->body, b->body);
  lovrHingeJointSetAnchor(joint, anchor);
  lovrHingeJointSetAxis(joint, axis);
  lovrRetain(joint);
  return joint;
}

void lovrHingeJointGetAnchors(HingeJoint* joint, float anchor1[3], float anchor2[3]) {
  dReal anchor[4];
  dJointGetHingeAnchor(joint->id, anchor);
  anchor1[0] = anchor[0];
  anchor1[1] = anchor[1];
  anchor1[2] = anchor[2];
  dJointGetHingeAnchor2(joint->id, anchor);
  anchor2[0] = anchor[0];
  anchor2[1] = anchor[1];
  anchor2[2] = anchor[2];
}

void lovrHingeJointSetAnchor(HingeJoint* joint, float anchor[3]) {
  dJointSetHingeAnchor(joint->id, anchor[0], anchor[1], anchor[2]);
}

void lovrHingeJointGetAxis(HingeJoint* joint, float axis[3]) {
  dReal daxis[4];
  dJointGetHingeAxis(joint->id, daxis);
  axis[0] = daxis[0];
  axis[1] = daxis[1];
  axis[2] = daxis[2];
}

void lovrHingeJointSetAxis(HingeJoint* joint, float axis[3]) {
  dJointSetHingeAxis(joint->id, axis[0], axis[1], axis[2]);
}

float lovrHingeJointGetAngle(HingeJoint* joint) {
  return dJointGetHingeAngle(joint->id);
}

float lovrHingeJointGetLowerLimit(HingeJoint* joint) {
  return dJointGetHingeParam(joint->id, dParamLoStop);
}

void lovrHingeJointSetLowerLimit(HingeJoint* joint, float limit) {
  dJointSetHingeParam(joint->id, dParamLoStop, limit);
}

float lovrHingeJointGetUpperLimit(HingeJoint* joint) {
  return dJointGetHingeParam(joint->id, dParamHiStop);
}

void lovrHingeJointSetUpperLimit(HingeJoint* joint, float limit) {
  dJointSetHingeParam(joint->id, dParamHiStop, limit);
}

SliderJoint* lovrSliderJointCreate(Collider* a, Collider* b, float axis[3]) {
  lovrCheck(a->world == b->world, "Joint bodies must exist in the same world");
  SliderJoint* joint = lovrCalloc(sizeof(SliderJoint));
  joint->ref = 1;
  joint->type = JOINT_SLIDER;
  joint->id = dJointCreateSlider(a->world->id, 0);
  dJointSetData(joint->id, joint);
  dJointAttach(joint->id, a->body, b->body);
  lovrSliderJointSetAxis(joint, axis);
  lovrRetain(joint);
  return joint;
}

void lovrSliderJointGetAxis(SliderJoint* joint, float axis[3]) {
  dReal daxis[4];
  dJointGetSliderAxis(joint->id, axis);
  axis[0] = daxis[0];
  axis[1] = daxis[1];
  axis[2] = daxis[2];
}

void lovrSliderJointSetAxis(SliderJoint* joint, float axis[3]) {
  dJointSetSliderAxis(joint->id, axis[0], axis[1], axis[2]);
}

float lovrSliderJointGetPosition(SliderJoint* joint) {
  return dJointGetSliderPosition(joint->id);
}

float lovrSliderJointGetLowerLimit(SliderJoint* joint) {
  return dJointGetSliderParam(joint->id, dParamLoStop);
}

void lovrSliderJointSetLowerLimit(SliderJoint* joint, float limit) {
  dJointSetSliderParam(joint->id, dParamLoStop, limit);
}

float lovrSliderJointGetUpperLimit(SliderJoint* joint) {
  return dJointGetSliderParam(joint->id, dParamHiStop);
}

void lovrSliderJointSetUpperLimit(SliderJoint* joint, float limit) {
  dJointSetSliderParam(joint->id, dParamHiStop, limit);
}
