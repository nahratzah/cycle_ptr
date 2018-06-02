#include <cycle_ptr/cycle_ptr.h>
#include "UnitTest++/UnitTest++.h"

using namespace cycle_ptr;

class create_destroy_check {
 public:
  constexpr create_destroy_check(bool* destroyed) noexcept
  : destroyed(destroyed)
  {}

  ~create_destroy_check() {
    if (destroyed != nullptr) {
      CHECK(!*destroyed); // Check that we're only destroyed once.
      *destroyed = true;
    }
  }

 private:
  bool* destroyed = nullptr;
};

class owner
: public create_destroy_check
{
 public:
  owner(bool* destroyed_owner, bool* destroyed_target)
  : create_destroy_check(destroyed_owner),
    target(make_cycle<create_destroy_check>(destroyed_target))
  {}

  explicit owner(bool* destroyed_owner)
  : create_destroy_check(destroyed_owner)
  {}

  cycle_member_ptr<create_destroy_check> target;
};

TEST(constructor) {
  bool owner_destroyed = false;
  bool target_destroyed = false;
  CHECK(make_cycle<owner>(&owner_destroyed, &target_destroyed)->target != nullptr);

  CHECK(owner_destroyed);
  CHECK(target_destroyed);
}

TEST(assignment) {
  bool owner_destroyed = false;
  bool target_destroyed = false;
  cycle_gptr<owner> ptr_1 = make_cycle<owner>(&owner_destroyed);

  ptr_1->target = make_cycle<create_destroy_check>(&target_destroyed);
  CHECK(ptr_1->target != nullptr);
  CHECK(!owner_destroyed);
  CHECK(!target_destroyed);

  ptr_1 = nullptr;
  CHECK(owner_destroyed);
  CHECK(target_destroyed);
}

TEST(self_reference) {
  bool destroyed = false;
  cycle_gptr<owner> ptr = make_cycle<owner>(&destroyed);
  ptr->target = ptr;

  CHECK(!destroyed);
  ptr = nullptr;
  CHECK(destroyed);
}

TEST(cycle) {
  bool first_destroyed = false;
  bool second_destroyed = false;
  cycle_gptr<owner> ptr_1 = make_cycle<owner>(&first_destroyed);
  cycle_gptr<owner> ptr_2 = make_cycle<owner>(&second_destroyed);
  ptr_1->target = ptr_2;
  ptr_2->target = ptr_1;

  REQUIRE CHECK_EQUAL(ptr_2, ptr_1->target);
  REQUIRE CHECK_EQUAL(ptr_1, ptr_2->target);

  CHECK(!first_destroyed);
  CHECK(!second_destroyed);

  ptr_1 = nullptr;
  CHECK(!first_destroyed);
  CHECK(!second_destroyed);

  ptr_2 = nullptr;
  CHECK(first_destroyed);
  CHECK(second_destroyed);
}

int main() {
  return UnitTest::RunAllTests();
}
