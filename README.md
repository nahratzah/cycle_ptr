# Cycle Pointer library

This library is a smart pointer library, that allows for cycles in
datastructures.

Basically, if you have a case where:

    class A;
    class B;

    class A {
    public:
      std::shared_ptr<B> b;
    };

    class B {
    public:
      std::shared_ptr<A> a;
    };

    int main() {
      std::shared_ptr<A> a_ptr = std::make_shared<A>();
      std::shared_ptr<B> b_ptr = std::make_shared<B>();

      a_ptr->b = b_ptr;
      b_ptr->a = a_ptr;

      a_ptr.reset();
      b_ptr.reset();
      // We now leaked A and B!
    }

is what you need, but you want to avoid the resulting memory leak, this library
may offer a solution.

The equivalent code in this library would be:

    class A;
    class B;

    class A {
    public:
      cycle_ptr::cycle_member_ptr<B> b;
    };

    class B {
    public:
      cycle_ptr::cycle_member_ptr<A> a;
    };

    int main() {
      cycle_ptr::cycle_gptr<A> a_ptr = cycle_ptr::make_cycle<A>();
      cycle_ptr::cycle_gptr<B> b_ptr = cycle_ptr::make_cycle<B>();

      a_ptr->b = b_ptr;
      b_ptr->a = a_ptr;

      a_ptr.reset();
      b_ptr.reset();
      // A and B are now unreachable, so cycle_ptr cleans them up properly.
    }

## Reference

Doxygen output is [viewable online](https://www.stack.nl/~ariane/cycle_ptr/).

## Overview

This library contains three pointers, which form the core of its interface.

1. ``cycle_ptr::cycle_member_ptr`` represents a relationship between two
   objects.
2. ``cycle_ptr::cycle_gptr`` represents a global pointer
   (that's what the ``g`` stands for... I should not be allowed to come up
   with names).
   This pointer is also used for function arguments and variables at function
   scope.
3. ``cycle_ptr::cycle_weak_ptr`` represents a weak pointer to an object.

``cycle_ptr::cycle_member_ptr`` and ``cycle_ptr::cycle_gptr`` operate similar
to ``std::shared_ptr``.
``cycle_ptr::cycle_weak_ptr`` is the equivalent of ``std::weak_ptr``.

## Dealing With Collections

Consider a collection: ``std::vector<cycle_ptr::cycle_gptr<MyClass>>``.

This collection is not suitable for use inside an object that participates
in the cycle\_ptr graph, as the link would not be modeled.

The alternative is to use ``std::vector<cycle_ptr::cycle_member_ptr<MyClass>>``,
but this would not be usable outside of an object participating in the graph.

The solution to this, is to use ``cycle_ptr::cycle_allocator<Alloc>``.
This allocator adapts an allocator ``Alloc``, by allowing cycle\_member\_ptr
to deduce its owner at construction.

    class MyClass;

    using MyClassVector = std::vector<
        cycle_ptr::cycle_gptr<MyClass>,
        cycle_ptr::cycle_allocator<std::allocator<cycle_ptr::cycle_gptr<MyClass>>>>;

    class MyClass
    : public cycle_ptr::cycle_base // Used as argument to allocator.
    {
      // Vector instantiated with allocator that enforces ownership from *this.
      MyClassVector data = MyClassVector(MyClassVector::allocator_type(*this));
    };

    // Create a MyClassVector that does not have an owner object.
    MyClassVector notAMember = MyClassVector(MyClassVector::allocator_type(cycle_ptr::unowned_cycle));

    void example(cycle_ptr::cycle_gptr<MyClass> ptr) {
      MyClass.data = notAMember; // Using copy assignment from std::vector.
    }

With this allocator, copy construction should *always* supply an allocator
such that ownership is explicitly stated.
While move constructor silently succeeds, care should be taken not to break
ownership rules.

The vector in this example uses pointers to demonstrate usage, but it works
equally well with structs or classes containing member pointers.

## Configuring

The library allows for limited control of the GC operations, using
``cycle_ptr::gc_operation`` and related functions.
