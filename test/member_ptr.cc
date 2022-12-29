#include <cycle_ptr/cycle_ptr.h>
#include <cycle_ptr/allocator.h>
#include "UnitTest++/UnitTest++.h"
#include <vector>

using namespace cycle_ptr;

class create_destroy_check {
 public:
  constexpr create_destroy_check() noexcept = default;

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

class owner_of_collection
: public cycle_base
{
 public:
  using vector_type = std::vector<
      cycle_member_ptr<create_destroy_check>,
      cycle_allocator<std::allocator<cycle_member_ptr<create_destroy_check>>>>;

  owner_of_collection()
  : data(vector_type::allocator_type(*this))
  {}

  owner_of_collection(const owner_of_collection& y)
  : data(y.data, vector_type::allocator_type(*this))
  {}

  owner_of_collection(owner_of_collection&& y)
  : data(std::move(y.data), vector_type::allocator_type(*this))
  {}

  auto operator=(const owner_of_collection& y)
  -> owner_of_collection& {
    data = y.data;
    return *this;
  }

  auto operator=(owner_of_collection&& y)
  -> owner_of_collection& {
    data = std::move(y.data);
    return *this;
  }

  template<typename Iter>
  owner_of_collection(Iter b, Iter e)
  : data(b, e, vector_type::allocator_type(*this))
  {}

  vector_type data;
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

TEST(null_pointee) {
  bool owner_destroyed = false;
  cycle_gptr<owner> ptr_1 = make_cycle<owner>(&owner_destroyed);

  REQUIRE CHECK(ptr_1->target == nullptr);
  CHECK(!owner_destroyed);

  ptr_1 = nullptr;
  CHECK(owner_destroyed);
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

TEST(move_seq) {
  std::vector<cycle_gptr<create_destroy_check>> pointers;
  std::generate_n(
      std::back_inserter(pointers),
      10'000,
      []() {
        return make_cycle<create_destroy_check>();
      });

  auto ooc = make_cycle<owner_of_collection>(pointers.cbegin(), pointers.cend());
}

TEST(expired_can_assign) {
  struct testclass {
    bool* td_ptr;
    cycle_member_ptr<create_destroy_check> ptr;

    explicit testclass(bool* td_ptr) : td_ptr(td_ptr) {}
    ~testclass() { // Test runs during the destructor.
      ptr = make_cycle<create_destroy_check>(td_ptr);
      CHECK_EQUAL(nullptr, ptr);
    }
  };

  bool destroyed = false;
  auto tc = make_cycle<testclass>(&destroyed);
  REQUIRE CHECK(tc != nullptr);
  tc.reset();
  REQUIRE CHECK(tc == nullptr);

  CHECK(destroyed);
}

TEST(expired_can_reset) {
  struct testclass {
    cycle_member_ptr<int> ptr;

    testclass()
    : ptr(make_cycle<int>())
    {}

    ~testclass() { // Test runs during the destructor.
      CHECK_EQUAL(nullptr, ptr);
      ptr.reset(); // We don't guarantee a specific moment of destruction.
      CHECK_EQUAL(nullptr, ptr);
    }
  };

  auto tc = make_cycle<testclass>();
  REQUIRE CHECK(tc != nullptr);
  tc.reset();
  REQUIRE CHECK(tc == nullptr);
}

TEST(expired_can_create_gptr_but_wont_resurrect) {
  struct testclass {
    cycle_gptr<int>& gptr;
    cycle_member_ptr<int> ptr;

    testclass(cycle_gptr<int>& gptr)
    : gptr(gptr),
      ptr(make_cycle<int>())
    {}

    ~testclass() { // Test runs during the destructor.
      gptr = ptr; // Because `ptr` is expired, gptr will be set to null.
    }
  };

  cycle_gptr<int> gptr = make_cycle<int>(42);
  auto tc = make_cycle<testclass>(gptr);
  REQUIRE CHECK(tc != nullptr);
  tc.reset();
  REQUIRE CHECK(tc == nullptr);
  CHECK_EQUAL(nullptr, gptr);
}

int main() {
  return UnitTest::RunAllTests();
}
