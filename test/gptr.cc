#include <cycle_ptr.h>
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

struct csc_container {
  constexpr csc_container(bool* destroyed) noexcept
  : data(destroyed)
  {}

  create_destroy_check data;
  int foo = 4;
};

TEST(gptr_constructor) {
  CHECK(make_cycle<int>(4) != nullptr);
}

TEST(destructor) {
  bool destroyed = false;
  cycle_gptr<create_destroy_check> ptr =
      make_cycle<create_destroy_check>(&destroyed);
  REQUIRE CHECK(ptr != nullptr);

  CHECK(!destroyed);
  ptr = nullptr;
  CHECK(destroyed);
}

TEST(share) {
  bool destroyed = false;
  cycle_gptr<create_destroy_check> ptr_1 =
      make_cycle<create_destroy_check>(&destroyed);
  cycle_gptr<create_destroy_check> ptr_2 = ptr_1;
  REQUIRE CHECK(ptr_1 != nullptr);
  REQUIRE CHECK(ptr_2 != nullptr);

  CHECK(!destroyed);
  ptr_1 = nullptr;
  CHECK(!destroyed);
  ptr_2 = nullptr;
  CHECK(destroyed);
}

TEST(alias) {
  bool destroyed = false;
  cycle_gptr<csc_container> ptr_1 =
      make_cycle<csc_container>(&destroyed);
  REQUIRE CHECK(ptr_1 != nullptr);
  auto alias = cycle_gptr<int>(ptr_1, &ptr_1->foo);
  REQUIRE CHECK(alias != nullptr);

  const int*const foo_addr = &ptr_1->foo;

  CHECK_EQUAL(foo_addr, alias.get());
  ptr_1 = nullptr;
  CHECK_EQUAL(foo_addr, alias.get());
  CHECK(!destroyed);

  alias = nullptr;
  CHECK(destroyed);
}
