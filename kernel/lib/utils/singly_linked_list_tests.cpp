// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <app/tests.h>
#include <err.h>
#include <unittest.h>
#include <utils/intrusive_single_list.h>
#include <utils/ref_counted.h>
#include <utils/ref_ptr.h>
#include <utils/unique_ptr.h>

namespace utils {

template <typename PtrType>
class ObjTypeBase : public SinglyLinkedListable<PtrType> {
public:
    struct OtherListTraits {
        using PtrTraits = internal::ContainerPtrTraits<PtrType>;
        static SinglyLinkedListNodeState<PtrType>& node_state(typename PtrTraits::RefType obj) {
            return obj.other_list_node_state_;
        }
    };

    explicit ObjTypeBase(size_t val) : val_(val) { live_obj_count_++; }
    ~ObjTypeBase() { live_obj_count_--; }

    size_t value() const { return val_; }
    const void* raw_ptr() const { return this; }

    static size_t live_obj_count() { return live_obj_count_; }
    static void ResetLiveObjCount() { live_obj_count_ = 0; }

private:
    static size_t live_obj_count_;

    size_t val_;
    SinglyLinkedListNodeState<PtrType> other_list_node_state_;
};

template <typename PtrType>
size_t ObjTypeBase<PtrType>::live_obj_count_ = 0;

struct UnmanagedPtrTraits {
    class ObjType;
    using PtrType      = ObjType*;
    using ConstPtrType = const ObjType*;
    using ListType     = SinglyLinkedList<PtrType>;

    class ObjType : public ObjTypeBase<PtrType> {
    public:
        ObjType(size_t val) : ObjTypeBase(val) { }
    };

    static PtrType CreateObject(size_t value) { return new ObjType(value); }

    // Unmanaged pointers never get cleared when being moved or transferred.
    static inline PtrType& Transfer(PtrType& ptr)       { return ptr; }
    static bool WasTransferred(const ConstPtrType& ptr) { return ptr != nullptr; }
    static bool WasMoved (const ConstPtrType& ptr)      { return ptr != nullptr; }
};

struct UniquePtrTraits {
    class ObjType;
    using PtrType      = ::utils::unique_ptr<ObjType>;
    using ConstPtrType = const PtrType;
    using ListType     = SinglyLinkedList<PtrType>;

    class ObjType : public ObjTypeBase<PtrType> {
    public:
        ObjType(size_t val) : ObjTypeBase(val) { }
    };

    static PtrType CreateObject(size_t value) { return PtrType(new ObjType(value)); }

    // Unique pointers always get cleared when being moved or transferred.
    static inline PtrType&& Transfer(PtrType& ptr)      { return utils::move(ptr); }
    static bool WasTransferred(const ConstPtrType& ptr) { return ptr == nullptr; }
    static bool WasMoved (const ConstPtrType& ptr)      { return ptr == nullptr; }
};

struct RefPtrTraits {
    class ObjType;
    using PtrType      = ::utils::RefPtr<ObjType>;
    using ConstPtrType = const PtrType;
    using ListType     = SinglyLinkedList<PtrType>;

    class ObjType : public ObjTypeBase<PtrType>,
                    public RefCounted<ObjType> {
    public:
        ObjType(size_t val) : ObjTypeBase(val) { }
    };

    static PtrType CreateObject(size_t value) { return AdoptRef(new ObjType(value)); }

    // RefCounted pointers do not get cleared when being transferred, but do get
    // cleared when being moved.
    static inline PtrType& Transfer(PtrType& ptr)       { return ptr; }
    static bool WasTransferred(const ConstPtrType& ptr) { return ptr != nullptr; }
    static bool WasMoved (const ConstPtrType& ptr)      { return ptr == nullptr; }
};

template <typename Traits>
class TestEnvironmentBase {
public:
    using ObjType   = typename Traits::ObjType;
    using PtrType   = typename Traits::PtrType;
    using ListType  = typename Traits::ListType;
    using PtrTraits = typename ListType::PtrTraits;

protected:
    PtrType CreateTrackedObject(size_t ndx, size_t value, bool ref_held) {
        ASSERT(ndx < OBJ_COUNT);
        DEBUG_ASSERT(!objects_[ndx]);

        PtrType ret = Traits::CreateObject(value);
        objects_[ndx] = PtrTraits::GetRaw(ret);
        DEBUG_ASSERT(objects_[ndx]);

        if (ref_held)
            refs_held_++;

        return utils::move(ret);
    }

    static constexpr size_t OBJ_COUNT = 17;
    ListType  list_;
    ObjType*  objects_[OBJ_COUNT] = { nullptr };
    size_t    refs_held_ = 0;
};

template <typename Traits>
class TestEnvironmentSpecialized;

template <>
class TestEnvironmentSpecialized<UnmanagedPtrTraits> :
    public TestEnvironmentBase<UnmanagedPtrTraits> {
protected:
    void ReleaseObject(size_t ndx) {
        ASSERT(ndx < OBJ_COUNT);
        if (objects_[ndx]) {
            delete objects_[ndx];
            objects_[ndx] = nullptr;
            refs_held_--;
        }
    }

    bool HoldingObject(size_t ndx) const {
        ASSERT(ndx < OBJ_COUNT);
        return !!objects_[ndx];
    }

    PtrType CreateTrackedObject(size_t ndx, size_t value, bool hold_ref = false) {
        using Base = TestEnvironmentBase<UnmanagedPtrTraits>;
        return Base::CreateTrackedObject(ndx, value, true);
    }
};

template <>
class TestEnvironmentSpecialized<UniquePtrTraits> :
    public TestEnvironmentBase<UniquePtrTraits> {
protected:
    void ReleaseObject(size_t ndx) {
        ASSERT(ndx < OBJ_COUNT);
        objects_[ndx] = nullptr;
    }

    bool HoldingObject(size_t ndx) const {
        ASSERT(ndx < OBJ_COUNT);
        return false;
    }

    PtrType CreateTrackedObject(size_t ndx, size_t value, bool hold_ref = false) {
        using Base = TestEnvironmentBase<UniquePtrTraits>;
        return Base::CreateTrackedObject(ndx, value, false);
    }
};

template <>
class TestEnvironmentSpecialized<RefPtrTraits> :
    public TestEnvironmentBase<RefPtrTraits> {
protected:
    PtrType CreateTrackedObject(size_t ndx, size_t value, bool hold_ref = false) {
        using Base = TestEnvironmentBase<RefPtrTraits>;
        PtrType ret = Base::CreateTrackedObject(ndx, value, hold_ref);

        if (hold_ref)
            refed_objects_[ndx] = ret;

        return utils::move(ret);
    }

    void ReleaseObject(size_t ndx) {
        ASSERT(ndx < OBJ_COUNT);
        objects_[ndx] = nullptr;
        if (refed_objects_[ndx]) {
            refed_objects_[ndx] = nullptr;
            refs_held_--;
        }
    }

    bool HoldingObject(size_t ndx) const {
        ASSERT(ndx < OBJ_COUNT);
        return refed_objects_[ndx] != nullptr;
    }

private:
    PtrType refed_objects_[OBJ_COUNT];
};


#define MAKE_TEST_THUNK(_test_name) \
static bool _test_name ## Test(void* ctx) { \
    TestEnvironment<Traits> env; \
    BEGIN_TEST; \
    EXPECT_TRUE(env._test_name(), ""); \
    EXPECT_TRUE(env.Reset(), ""); \
    END_TEST; \
}

template <typename Traits>
class TestEnvironment : public TestEnvironmentSpecialized<Traits> {
public:
    using ObjType   = typename TestEnvironmentBase<Traits>::ObjType;
    using ListType  = typename TestEnvironmentBase<Traits>::ListType;
    using PtrTraits = typename TestEnvironmentBase<Traits>::PtrTraits;
    using PtrType   = typename TestEnvironmentBase<Traits>::PtrType;

    ~TestEnvironment() { Reset(); }

    bool Reset() {
        BEGIN_TEST;

        list().clear();
        for (size_t i = 0; i < OBJ_COUNT; ++i)
            ReleaseObject(i);

        EXPECT_EQ(0u, refs_held(), "");
        refs_held() = 0;

        EXPECT_EQ(0u, ObjType::live_obj_count(), "");
        ObjTypeBase<PtrType>::ResetLiveObjCount();

        END_TEST;
    }

    bool Populate() {
        BEGIN_TEST;

        EXPECT_EQ(0U, ObjType::live_obj_count(), "");

        for (size_t i = 0; i < OBJ_COUNT; ++i) {
            size_t ndx = OBJ_COUNT - i - 1;
            EXPECT_EQ(i, list().size_slow(), "");

            // Don't hold a reference in the test environment for every 4th
            // object created.  Note, this only affects RefPtr tests.  Unmanaged
            // pointers always hold an unmanaged copy of the pointer (so it can
            // be cleaned up), while unique_ptr tests are not able to hold an
            // extra copy of the pointer (because it is unique)
            PtrType new_object = this->CreateTrackedObject(ndx, ndx, (i & 0x3));
            REQUIRE_NONNULL(new_object, "");
            EXPECT_EQ(new_object->raw_ptr(), objects()[ndx], "");

            // Alternate whether or not we move the pointer, or "transfer" it.
            // Transfering means different things for different pointer types.
            // For unmanaged, it just returns a reference to the pointer and
            // leaves the original unaltered.  For unique, it moves the pointer
            // (clearing the source).  For RefPtr, it makes a new RefPtr
            // instance, bumping the reference count in the process.
            if (i & 1) {
#if TEST_WILL_NOT_COMPILE || 0
                list().push_front(new_object);
#else
                list().push_front(Traits::Transfer(new_object));
#endif
                EXPECT_TRUE(Traits::WasTransferred(new_object), "");
            } else {
                list().push_front(utils::move(new_object));
                EXPECT_TRUE(Traits::WasMoved(new_object), "");
            }
        }

        EXPECT_EQ(OBJ_COUNT, list().size_slow(), "");
        EXPECT_EQ(OBJ_COUNT, ObjType::live_obj_count(), "");

        END_TEST;
    }

    bool Clear() {
        BEGIN_TEST;

        // Start by making some objects.
        REQUIRE_TRUE(Populate(), "");

        // Clear the list.  Afterwards, the number of live objects we have
        // should be equal to the number of references being held by the test
        // environment.
        list().clear();
        EXPECT_EQ(0u, list().size_slow(), "");
        EXPECT_EQ(refs_held(), ObjType::live_obj_count(), "");

        for (size_t i = 0; i < OBJ_COUNT; ++i) {
            EXPECT_NONNULL(objects()[i], "");

            // If our underlying object it still being kept alive by the test
            // environment, make sure that its next_ pointers have been properly
            // cleared out.
            if (HoldingObject(i)) {
                auto& ns = ListType::NodeTraits::node_state(*objects()[i]);
                EXPECT_NULL(PtrTraits::GetRaw(ns.next_), "");
            }
        }

        END_TEST;
    }

    bool IsEmpty() {
        BEGIN_TEST;

        EXPECT_TRUE(list().is_empty(), "");
        REQUIRE_TRUE(Populate(), "");
        EXPECT_FALSE(list().is_empty(), "");
        EXPECT_TRUE(Reset(), "");
        EXPECT_TRUE(list().is_empty(), "");

        END_TEST;
    }

    bool PopFront() {
        BEGIN_TEST;

        REQUIRE_TRUE(Populate(), "");

        // Remove elements using pop_front.  List should shrink each time we
        // remove an element, but the number of live objects should only shrink
        // when we let the last reference go out of scope.
        for (size_t i = 0; i < OBJ_COUNT; ++i) {
            size_t remaining = OBJ_COUNT - i;
            REQUIRE_TRUE(!list().is_empty(), "");
            EXPECT_EQ(remaining, ObjType::live_obj_count(), "");
            EXPECT_EQ(remaining, list().size_slow(), "");

            {
                // Pop the item and sanity check it against our tracking.
                PtrType tmp = list().pop_front();
                EXPECT_NONNULL(tmp, "");
                EXPECT_EQ(tmp->value(), i, "");
                EXPECT_EQ(objects()[i], tmp->raw_ptr(), "");

                // Make sure that the intrusive bookkeeping is up-to-date.
                auto& ns = ListType::NodeTraits::node_state(*tmp);
                EXPECT_NULL(ns.next_, "");

                // The list has shrunk, but the object should still be around.
                EXPECT_EQ(remaining, ObjType::live_obj_count(), "");
                EXPECT_EQ(remaining - 1, list().size_slow(), "");
            }

            // If we were not holding onto the object using the test
            // environment's tracking, the live object count should have
            // dropped.  Otherwise, it should remain the same.
            if (!HoldingObject(i))
                EXPECT_EQ(remaining - 1, ObjType::live_obj_count(), "");
            else
                EXPECT_EQ(remaining, ObjType::live_obj_count(), "");

            // Let go of the object and verify that it has now gone away.
            ReleaseObject(i);
            EXPECT_EQ(remaining - 1, ObjType::live_obj_count(), "");
        }

        // List should be empty now.  Popping anything else should result in a
        // null pointer.
        EXPECT_TRUE(list().is_empty(), "");
        PtrType should_be_null = list().pop_front();
        EXPECT_NULL(should_be_null, "");

        END_TEST;
    }

    bool EraseNext() {
        BEGIN_TEST;

        REQUIRE_TRUE(Populate(), "");

        // Remove as many elements as we can using erase_next.
        auto iter = list().begin();
        for (size_t i = 1; i < OBJ_COUNT; ++i) {
            size_t remaining = OBJ_COUNT - i + 1;
            REQUIRE_TRUE(!list().is_empty(), "");
            REQUIRE_TRUE(iter != list().end(), "");
            EXPECT_EQ(remaining, ObjType::live_obj_count(), "");
            EXPECT_EQ(remaining, list().size_slow(), "");

            {
                // Erase the item and sanity check it against our tracking.
                PtrType tmp = list().erase_next(iter);
                EXPECT_NONNULL(tmp, "");
                EXPECT_EQ(tmp->value(), i, "");
                EXPECT_EQ(objects()[i], tmp->raw_ptr(), "");

                // Make sure that the intrusive bookkeeping is up-to-date.
                auto& ns = ListType::NodeTraits::node_state(*tmp);
                EXPECT_NULL(ns.next_, "");

                // The list has shrunk, but the object should still be around.
                EXPECT_EQ(remaining, ObjType::live_obj_count(), "");
                EXPECT_EQ(remaining - 1, list().size_slow(), "");
            }

            // If we were not holding onto the object using the test
            // environment's tracking, the live object count should have
            // dropped.  Otherwise, it should remain the same.
            if (!HoldingObject(i))
                EXPECT_EQ(remaining - 1, ObjType::live_obj_count(), "");
            else
                EXPECT_EQ(remaining, ObjType::live_obj_count(), "");

            // Let go of the object and verify that it has now gone away.
            ReleaseObject(i);
            EXPECT_EQ(remaining - 1, ObjType::live_obj_count(), "");
        }

        // Iterator should now be one away from the end, and there should be one
        // object left
        EXPECT_EQ(1u, ObjType::live_obj_count(), "");
        EXPECT_EQ(1u, list().size_slow(), "");
        EXPECT_TRUE(iter != list().end(), "");
        iter++;
        EXPECT_TRUE(iter == list().end(), "");

        END_TEST;
    }

    bool Iterate() {
        BEGIN_TEST;

        // Start by making some objects.
        REQUIRE_TRUE(Populate(), "");
        EXPECT_EQ(OBJ_COUNT, list().size_slow(), "");

        // Iterate using normal begin/end
        size_t i = 0;
        for (auto iter = list().begin(); iter != list().end(); ) {
            // Exercise both -> and * dereferencing
            EXPECT_EQ(objects()[i],   iter->raw_ptr(), "");
            EXPECT_EQ(objects()[i], (*iter).raw_ptr(), "");
            EXPECT_EQ(i,   iter->value(), "");
            EXPECT_EQ(i, (*iter).value(), "");

            // Exercise both pre and postfix increment
            if ((i++) & 1) iter++;
            else           ++iter;
        }

        // Iterate using normal const begin/end
        i = 0;
        for (auto iter = list().cbegin(); iter != list().cend(); ) {
            // Exercise both -> and * dereferencing
            EXPECT_EQ(objects()[i],   iter->raw_ptr(), "");
            EXPECT_EQ(objects()[i], (*iter).raw_ptr(), "");
            EXPECT_EQ(i,   iter->value(), "");
            EXPECT_EQ(i, (*iter).value(), "");

            // Exercise both pre and postfix increment
            if ((i++) & 1) iter++;
            else           ++iter;
        }

        // Iterate using the range-based for loop syntax
        i = 0;
        for (auto& obj : list()) {
            EXPECT_EQ(objects()[i], &obj, "");
            EXPECT_EQ(objects()[i], obj.raw_ptr(), "");
            EXPECT_EQ(i, obj.value(), "");
            i++;
        }

        // Iterate using the range-based for loop syntax over const references.
        i = 0;
        for (const auto& obj : list()) {
            EXPECT_EQ(objects()[i], &obj, "");
            EXPECT_EQ(objects()[i], obj.raw_ptr(), "");
            EXPECT_EQ(i, obj.value(), "");
            i++;
        }

        {
            // Advancing iter past the end of the list should be a no-op.  Check
            // both pre and post-fix.
            auto iter = list().end();
            ++iter;
            EXPECT_TRUE(iter == list().end(), "");

            // We know that the iterator  is already at the end of the list, but
            // perform the explicit assignment in order to check that the assignment
            // operator is working (the previous version actually exercises the copy
            // constructor or the explicit rvalue constructor, if supplied)
            iter = list().end();
            iter++;
            EXPECT_TRUE(iter == list().end(), "");
        }

        {
            // Same checks as before, but this time with the const_iterator
            // form.
            auto iter = list().cend();
            ++iter;
            EXPECT_TRUE(iter == list().cend(), "");

            iter = list().cend();
            iter++;
            EXPECT_TRUE(iter == list().cend(), "");
        }

        END_TEST;
    }

    bool DoInsertAfter(typename ListType::iterator& iter, size_t pos) {
        BEGIN_TEST;

        EXPECT_EQ(ObjType::live_obj_count(), list().size_slow(), "");
        EXPECT_TRUE(iter != list().end(), "");

        size_t orig_list_len = ObjType::live_obj_count();
        size_t orig_iter_pos = iter->value();

        REQUIRE_LT(orig_iter_pos, OBJ_COUNT, "");
        EXPECT_EQ(objects()[orig_iter_pos], iter->raw_ptr(), "");

        PtrType new_object = this->CreateTrackedObject(pos, pos, true);
        REQUIRE_NONNULL(new_object, "");
        EXPECT_EQ(new_object->raw_ptr(), objects()[pos], "");

        if (pos & 1) {
#if TEST_WILL_NOT_COMPILE || 0
            list().insert_after(iter, new_object);
#else
            list().insert_after(iter, Traits::Transfer(new_object));
#endif
            EXPECT_TRUE(Traits::WasTransferred(new_object), "");
        } else {
            list().insert_after(iter, utils::move(new_object));
            EXPECT_TRUE(Traits::WasMoved(new_object), "");
        }

        // List and number of live object should have grown.
        EXPECT_EQ(orig_list_len + 1, ObjType::live_obj_count(), "");
        EXPECT_EQ(orig_list_len + 1, list().size_slow(), "");

        // The iterator should not have moved yet.
        EXPECT_TRUE(iter != list().end(), "");
        EXPECT_EQ(objects()[orig_iter_pos], iter->raw_ptr(), "");
        EXPECT_EQ(orig_iter_pos, iter->value(), "");

        END_TEST;
    }

    bool InsertAfter() {
        BEGIN_TEST;

        // In order to insert_after, we need at least one object already in the
        // list.  Use push_front to make one.
        EXPECT_EQ(0u, ObjType::live_obj_count(), "");
        EXPECT_EQ(0U, list().size_slow(), "");
        EXPECT_TRUE(list().is_empty(), "");
        list().push_front(utils::move(this->CreateTrackedObject(0, 0, true)));

        // Insert some elements after the last element list.
        static constexpr size_t END_INSERT_COUNT = 2;
        // TODO(johngro) : static assert count/insert count here
        auto iter = list().begin();
        for (size_t i = (OBJ_COUNT - END_INSERT_COUNT); i < OBJ_COUNT; ++i) {
            EXPECT_TRUE(DoInsertAfter(iter, i), "");

            // Now that we have inserted after, we should be able to advance the
            // iterator to what we just inserted.
            iter++;

            EXPECT_TRUE(iter != list().end(), "");
            EXPECT_EQ(objects()[i], iter->raw_ptr(), "");
            EXPECT_EQ(objects()[i], (*iter).raw_ptr(), "");
            EXPECT_EQ(i, iter->value(), "");
            EXPECT_EQ(i, (*iter).value(), "");
        }

        // Advancing iter at this point should bring it to the end.
        EXPECT_TRUE(iter != list().end(), "");
        iter++;
        EXPECT_TRUE(iter == list().end(), "");

        // Reset the iterator to the first element in the list, and test
        // inserting between elements instead of at the end.  To keep the
        // final list in order, we need to insert in reverse order and to not
        // advance the iterator in the process.
        iter = list().begin();
        for (size_t i = (OBJ_COUNT - END_INSERT_COUNT - 1); i > 0; --i) {
            EXPECT_TRUE(DoInsertAfter(iter, i), "");
        }
        EXPECT_TRUE(iter != list().end(), "");

        // Check to make sure the list has the expected number of elements, and
        // that they are in the proper order.
        EXPECT_EQ(OBJ_COUNT, ObjType::live_obj_count(), "");
        EXPECT_EQ(OBJ_COUNT, list().size_slow(), "");

        size_t i = 0;
        for (const auto& obj : list()) {
            EXPECT_EQ(objects()[i], &obj, "");
            EXPECT_EQ(objects()[i], obj.raw_ptr(), "");
            EXPECT_EQ(i, obj.value(), "");
            i++;
        }

        END_TEST;
    }

    bool Swap() {
        BEGIN_TEST;
        size_t i;

        {
            ListType other_list; // Make an empty list.
            REQUIRE_TRUE(Populate(), ""); // Fill the internal list with stuff.

            // Sanity check, swap, then check again.
            EXPECT_EQ(OBJ_COUNT, ObjType::live_obj_count(), "");
            EXPECT_FALSE(list().is_empty(), "");
            EXPECT_EQ(OBJ_COUNT, list().size_slow(), "");
            EXPECT_TRUE(other_list.is_empty(), "");

            i = 0;
            for (const auto& obj : list()) {
                EXPECT_EQ(i, obj.value(), "");
                i++;
            }

            list().swap(other_list);

            EXPECT_EQ(OBJ_COUNT, ObjType::live_obj_count(), "");
            EXPECT_FALSE(other_list.is_empty(), "");
            EXPECT_EQ(OBJ_COUNT, other_list.size_slow(), "");
            EXPECT_TRUE(list().is_empty(), "");

            i = 0;
            for (const auto& obj : other_list) {
                EXPECT_EQ(i, obj.value(), "");
                i++;
            }

            // Swap back to check the case where list() was empty, but other_list
            // had elements.
            list().swap(other_list);

            EXPECT_EQ(OBJ_COUNT, ObjType::live_obj_count(), "");
            EXPECT_FALSE(list().is_empty(), "");
            EXPECT_EQ(OBJ_COUNT, list().size_slow(), "");
            EXPECT_TRUE(other_list.is_empty(), "");

            i = 0;
            for (const auto& obj : list()) {
                EXPECT_EQ(i, obj.value(), "");
                i++;
            }

            // Reset;
            EXPECT_TRUE(Reset(), "");
        }

        // Make a new other_list, this time with some stuff in it.
        EXPECT_EQ(0u, ObjType::live_obj_count(), "");
        {
            ListType other_list; // Make an empty list.
            REQUIRE_TRUE(Populate(), ""); // Fill the internal list with stuff.

            static constexpr size_t OTHER_COUNT = 5;
            static constexpr size_t OTHER_START = 50000;
            ObjType* raw_ptrs[OTHER_COUNT];

            for (i = 0; i < OTHER_COUNT; ++i) {
                PtrType ptr = Traits::CreateObject(OTHER_START + OTHER_COUNT - i - 1);
                raw_ptrs[i] = PtrTraits::GetRaw(ptr);
                other_list.push_front(utils::move(ptr));
            }

            // Sanity check
            EXPECT_EQ(OBJ_COUNT + OTHER_COUNT, ObjType::live_obj_count(), "");
            EXPECT_EQ(OBJ_COUNT, list().size_slow(), "");
            EXPECT_EQ(OTHER_COUNT, other_list.size_slow(), "");

            i = 0;
            for (const auto& obj : list()) {
                EXPECT_EQ(i, obj.value(), "");
                i++;
            }

            i = OTHER_START;
            for (const auto& obj : other_list) {
                EXPECT_EQ(i, obj.value(), "");
                i++;
            }

            // Swap and sanity check again
            list().swap(other_list);

            EXPECT_EQ(OBJ_COUNT + OTHER_COUNT, ObjType::live_obj_count(), "");
            EXPECT_EQ(OBJ_COUNT, other_list.size_slow(), "");
            EXPECT_EQ(OTHER_COUNT, list().size_slow(), "");

            i = OTHER_START;
            for (const auto& obj : list()) {
                EXPECT_EQ(i, obj.value(), "");
                i++;
            }

            i = 0;
            for (const auto& obj : other_list) {
                EXPECT_EQ(i, obj.value(), "");
                i++;
            }

            // Swap back and sanity check again
            list().swap(other_list);

            EXPECT_EQ(OBJ_COUNT + OTHER_COUNT, ObjType::live_obj_count(), "");
            EXPECT_EQ(OBJ_COUNT, list().size_slow(), "");
            EXPECT_EQ(OTHER_COUNT, other_list.size_slow(), "");

            i = 0;
            for (const auto& obj : list()) {
                EXPECT_EQ(i, obj.value(), "");
                i++;
            }

            i = OTHER_START;
            for (const auto& obj : other_list) {
                EXPECT_EQ(i, obj.value(), "");
                i++;
            }

            // If we are testing unmanaged pointers clean them up.
            EXPECT_EQ(OBJ_COUNT + OTHER_COUNT, ObjType::live_obj_count(), "");
            other_list.clear();
            if (!PtrTraits::IsManaged) {
                EXPECT_EQ(OBJ_COUNT + OTHER_COUNT, ObjType::live_obj_count(), "");
                for (i = 0; i < OTHER_COUNT; ++i)
                    delete raw_ptrs[i];
            }
            EXPECT_EQ(OBJ_COUNT, ObjType::live_obj_count(), "");

            // Reset the internal state
            EXPECT_TRUE(Reset(), "");
            EXPECT_EQ(0u, ObjType::live_obj_count(), "");
        }


        END_TEST;
    }

    bool RvalueOps() {
        BEGIN_TEST;
        size_t i;

        // Populate the internal list.
        REQUIRE_TRUE(Populate(), "");
        EXPECT_EQ(OBJ_COUNT, ObjType::live_obj_count(), "");
        EXPECT_EQ(OBJ_COUNT, list().size_slow(), "");
        i = 0;
        for (const auto& obj : list()) {
            EXPECT_EQ(i, obj.value(), "");
            i++;
        }

        // Move its contents to a new list using the Rvalue constructor.
#if TEST_WILL_NOT_COMPILE || 0
        ListType other_list(list());
#else
        ListType other_list(utils::move(list()));
#endif
        EXPECT_EQ(OBJ_COUNT, ObjType::live_obj_count(), "");
        EXPECT_EQ(OBJ_COUNT, other_list.size_slow(), "");
        EXPECT_TRUE(list().is_empty(), "");
        i = 0;
        for (const auto& obj : other_list) {
            EXPECT_EQ(i, obj.value(), "");
            i++;
        }

        // Move the contents of the other list back to the internal list.  If we
        // are testing managed pointer types, put some objects into the internal
        // list first and make sure they get released.  Don't try this with
        // unmanaged pointers as it will trigger an assert if you attempt to
        // blow away a non-empty list via Rvalue assignment.
        static constexpr size_t EXTRA_COUNT = 5;
        size_t extras_added = 0;
        if (PtrTraits::IsManaged) {
            while (extras_added < EXTRA_COUNT)
                list().push_front(utils::move(Traits::CreateObject(extras_added++)));
        }

        // Sanity checks before the assignment
        EXPECT_EQ(OBJ_COUNT + extras_added, ObjType::live_obj_count(), "");
        EXPECT_EQ(extras_added, list().size_slow(), "");
        i = 1;
        for (const auto& obj : list()) {
            EXPECT_EQ(EXTRA_COUNT - i, obj.value(), "");
            i++;
        }

#if TEST_WILL_NOT_COMPILE || 0
        list() = other_list;
#else
        list() = utils::move(other_list);
#endif

        // other_list should now be empty, and we should have returned to our
        // starting, post-populated state.
        EXPECT_EQ(OBJ_COUNT, ObjType::live_obj_count(), "");
        EXPECT_EQ(OBJ_COUNT, list().size_slow(), "");
        EXPECT_TRUE(other_list.is_empty(), "");
        i = 0;
        for (const auto& obj : list()) {
            EXPECT_EQ(i, obj.value(), "");
            i++;
        }

        END_TEST;
    }

    bool TwoList() {
        BEGIN_TEST;

        // Start by populating the internal list.  We should end up with
        // OBJ_COUNT objects, but we may not be holding internal references to
        // all of them.
        REQUIRE_TRUE(Populate(), "");

        // Create the other type of list that ObjType can exist on and populate
        // it using push_front.
        SinglyLinkedList<PtrType, typename ObjType::OtherListTraits> other_list;
        for (auto iter = list().begin(); iter != list().end(); ++iter)
            other_list.push_front(utils::move(iter.CopyPointer()));

        // The two lists should be the same length, and nothing should have
        // changed about the live object count.
        EXPECT_EQ(OBJ_COUNT, ObjType::live_obj_count(), "");
        EXPECT_EQ(OBJ_COUNT, list().size_slow(), "");
        EXPECT_EQ(OBJ_COUNT, other_list.size_slow(), "");

        // other_list should be in the reverse order of list()
        auto other_iter = other_list.begin();
        for (const auto& obj : list()) {
            REQUIRE_FALSE(other_iter == other_list.end(), "");
            EXPECT_EQ(OBJ_COUNT - obj.value() - 1, other_iter->value(), "");
            ++other_iter;
        }
        EXPECT_TRUE(other_iter == other_list.end(), "");

        // Clear the internal list.  No objects should go away and the other
        // list should be un-affected
        list().clear();

        EXPECT_EQ(OBJ_COUNT, ObjType::live_obj_count(), "");
        EXPECT_EQ(0u, list().size_slow(), "");
        EXPECT_EQ(OBJ_COUNT, other_list.size_slow(), "");

        other_iter = other_list.begin();
        for (size_t i = 0; i < OBJ_COUNT; ++i) {
            REQUIRE_FALSE(other_iter == other_list.end(), "");
            EXPECT_EQ(OBJ_COUNT - i - 1, other_iter->value(), "");
            ++other_iter;
        }
        EXPECT_TRUE(other_iter == other_list.end(), "");

        // If we are testing a list of managed pointers, release our internal
        // references.  Again, no objects should go away (as they are being
        // referenced by other_list.  Note: Don't try this with an unmanaged
        // pointer.  "releasing" and unmanaged pointer in the context of the
        // TestEnvironment class means to return it to the heap, which is a Very
        // Bad thing if we still have a list refering to the objects which were
        // returned to the heap.
        if (PtrTraits::IsManaged) {
            for (size_t i = 0; i < OBJ_COUNT; ++i)
                ReleaseObject(i);

            EXPECT_EQ(OBJ_COUNT, ObjType::live_obj_count(), "");
            EXPECT_EQ(0u, refs_held(), "");
            EXPECT_EQ(OBJ_COUNT, other_list.size_slow(), "");
        }

        // Finally, clear() other_list and reset the internal state.  At this
        // point, all objects should have gone away.
        other_list.clear();
        EXPECT_TRUE(Reset(), "");

        EXPECT_EQ(0u, ObjType::live_obj_count(), "");
        EXPECT_EQ(0u, refs_held(), "");
        EXPECT_EQ(0u, list().size_slow(), "");
        EXPECT_EQ(0u, other_list.size_slow(), "");

        END_TEST;
    }

    bool EraseIf() {
        BEGIN_TEST;

        // Populate our list.
        REQUIRE_TRUE(Populate(), "");

        // Erase all of the even members
        size_t even_erased = 0;
        while (even_erased < OBJ_COUNT) {
            if (nullptr == list().erase_if([](const ObjType& obj) -> bool {
                    return !(obj.value() & 1);
                }))
                break;
            even_erased++;
        }

        static constexpr size_t EVEN_OBJ_COUNT = (OBJ_COUNT >> 1) + (OBJ_COUNT & 1);
        EXPECT_EQ(EVEN_OBJ_COUNT, even_erased, "");
        EXPECT_EQ(OBJ_COUNT, even_erased + list().size_slow(), "");
        for (const auto& obj : list())
            EXPECT_TRUE(obj.value() & 1, "");

        // Erase all of the odd members
        size_t odd_erased = 0;
        while (even_erased < OBJ_COUNT) {
            if (nullptr == list().erase_if([](const ObjType& obj) -> bool {
                    return obj.value() & 1;
                }))
                break;
            odd_erased++;
        }

        static constexpr size_t ODD_OBJ_COUNT = (OBJ_COUNT >> 1);
        EXPECT_EQ(ODD_OBJ_COUNT, odd_erased, "");
        EXPECT_EQ(OBJ_COUNT, even_erased + odd_erased, "");
        EXPECT_TRUE(list().is_empty(), "");

        END_TEST;
    }

    static bool ScopeTest(void* ctx) {
        BEGIN_TEST;

        // Make sure that both unique_ptrs and RefPtrs handle being moved
        // properly, and that lists of such pointers automatically clean up when
        // the list goes out of scope and destructs.  Note: Don't try this with
        // an unmanaged pointer.  Lists of unmanaged pointers will ASSERT if
        // they destruct with elements still in them.
        EXPECT_EQ(0U, ObjType::live_obj_count(), "");

        {  // Begin scope for list
            ListType list;

            for (size_t i = 0; i < OBJ_COUNT; ++i) {
                // Make a new object
                PtrType obj = Traits::CreateObject(i);
                EXPECT_NONNULL(obj, "");
                EXPECT_EQ(i + 1, ObjType::live_obj_count(), "");
                EXPECT_EQ(i, list.size_slow(), "");

                // Move it into the list
                list.push_front(utils::move(obj));
                EXPECT_NULL(obj, "");
                EXPECT_EQ(i + 1, ObjType::live_obj_count(), "");
                EXPECT_EQ(i + 1, list.size_slow(), "");
            }

            EXPECT_EQ(OBJ_COUNT, ObjType::live_obj_count(), "");
            EXPECT_EQ(OBJ_COUNT, list.size_slow(), "");
        }  // Let the list go out of scope and clean itself up..

        EXPECT_EQ(0U, ObjType::live_obj_count(), "");

        END_TEST;
    }

    MAKE_TEST_THUNK(Populate);
    MAKE_TEST_THUNK(Clear);
    MAKE_TEST_THUNK(IsEmpty);
    MAKE_TEST_THUNK(Iterate);
    MAKE_TEST_THUNK(InsertAfter);
    MAKE_TEST_THUNK(PopFront);
    MAKE_TEST_THUNK(EraseNext);
    MAKE_TEST_THUNK(Swap);
    MAKE_TEST_THUNK(RvalueOps);
    MAKE_TEST_THUNK(TwoList);
    MAKE_TEST_THUNK(EraseIf);

private:
    // Accessors for base class memebers so we don't have to type
    // this->base_member all of the time.
    using Sp   = TestEnvironmentSpecialized<Traits>;
    using Base = TestEnvironmentBase<Traits>;
    static constexpr size_t OBJ_COUNT = Base::OBJ_COUNT;

    ListType&  list()      { return this->list_; }
    ObjType**  objects()   { return this->objects_; }
    size_t&    refs_held() { return this->refs_held_; }

    void ReleaseObject(size_t ndx) { Sp::ReleaseObject(ndx); }
    bool HoldingObject(size_t ndx) const { return Sp::HoldingObject(ndx); }
};
#undef MAKE_TEST_THUNK

using UMTE = TestEnvironment<UnmanagedPtrTraits>;
using UPTE = TestEnvironment<UniquePtrTraits>;
using RPTE = TestEnvironment<RefPtrTraits>;
UNITTEST_START_TESTCASE(single_linked_list_tests)
UNITTEST("Populate (unmanaged)",    UMTE::PopulateTest)
UNITTEST("Populate (unique)",       UPTE::PopulateTest)
UNITTEST("Populate (RefPtr)",       RPTE::PopulateTest)

UNITTEST("Clear (unmanaged)",       UMTE::ClearTest)
UNITTEST("Clear (unique)",          UPTE::ClearTest)
UNITTEST("Clear (RefPtr)",          RPTE::ClearTest)

UNITTEST("IsEmpty (unmanaged)",     UMTE::IsEmptyTest)
UNITTEST("IsEmpty (unique)",        UPTE::IsEmptyTest)
UNITTEST("IsEmpty (RefPtr)",        RPTE::IsEmptyTest)

UNITTEST("Iterate (unmanaged)",     UMTE::IterateTest)
UNITTEST("Iterate (unique)",        UPTE::IterateTest)
UNITTEST("Iterate (RefPtr)",        RPTE::IterateTest)

UNITTEST("InsertAfter (unmanaged)", UMTE::InsertAfterTest)
UNITTEST("InsertAfter (unique)",    UPTE::InsertAfterTest)
UNITTEST("InsertAfter (RefPtr)",    RPTE::InsertAfterTest)

UNITTEST("PopFront (unmanaged)",    UMTE::PopFrontTest)
UNITTEST("PopFront (unique)",       UPTE::PopFrontTest)
UNITTEST("PopFront (RefPtr)",       RPTE::PopFrontTest)

UNITTEST("EraseNext (unmanaged)",   UMTE::EraseNextTest)
UNITTEST("EraseNext (unique)",      UPTE::EraseNextTest)
UNITTEST("EraseNext (RefPtr)",      RPTE::EraseNextTest)

UNITTEST("Swap (unmanaged)",        UMTE::SwapTest)
UNITTEST("Swap (unique)",           UPTE::SwapTest)
UNITTEST("Swap (RefPtr)",           RPTE::SwapTest)

UNITTEST("Rvalue Ops (unmanaged)",  UMTE::RvalueOpsTest)
UNITTEST("Rvalue Ops (unique)",     UPTE::RvalueOpsTest)
UNITTEST("Rvalue Ops (RefPtr)",     RPTE::RvalueOpsTest)

UNITTEST("TwoList (unmanaged)",     UMTE::TwoListTest)
#if TEST_WILL_NOT_COMPILE || 0
UNITTEST("TwoList (unique)",        UPTE::TwoListTest)
#endif
UNITTEST("TwoList (RefPtr)",        RPTE::TwoListTest)

UNITTEST("EraseIf (unmanaged)",     UMTE::EraseIfTest)
UNITTEST("EraseIf (unique)",        UPTE::EraseIfTest)
UNITTEST("EraseIf (RefPtr)",        RPTE::EraseIfTest)

UNITTEST("Scope (unique)",          UPTE::ScopeTest)
UNITTEST("Scope (RefPtr)",          RPTE::ScopeTest)
UNITTEST_END_TESTCASE(single_linked_list_tests,
                      "sll",
                      "Intrusive singly linked list tests.",
                      NULL, NULL);

}  // namespace utils

