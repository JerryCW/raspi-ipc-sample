#include <gtest/gtest.h>
#include "core/types.h"

#include <string>

// ============================================================
// ErrorCode enum values
// ============================================================

TEST(ErrorCode, GeneralRange) {
    EXPECT_EQ(static_cast<uint16_t>(sc::ErrorCode::OK), 0);
    EXPECT_EQ(static_cast<uint16_t>(sc::ErrorCode::Unknown), 1);
    EXPECT_EQ(static_cast<uint16_t>(sc::ErrorCode::InvalidArgument), 2);
    EXPECT_EQ(static_cast<uint16_t>(sc::ErrorCode::Timeout), 3);
    EXPECT_EQ(static_cast<uint16_t>(sc::ErrorCode::NotFound), 4);
}

TEST(ErrorCode, SubsystemRanges) {
    // Config 100-199
    EXPECT_EQ(static_cast<uint16_t>(sc::ErrorCode::ConfigFileNotFound), 100);
    EXPECT_EQ(static_cast<uint16_t>(sc::ErrorCode::ConfigPresetNotFound), 103);
    // Auth 200-299
    EXPECT_EQ(static_cast<uint16_t>(sc::ErrorCode::CertificateNotFound), 200);
    // Camera 300-399
    EXPECT_EQ(static_cast<uint16_t>(sc::ErrorCode::CameraDeviceNotFound), 300);
    // Pipeline 400-499
    EXPECT_EQ(static_cast<uint16_t>(sc::ErrorCode::EncoderNotAvailable), 402);
    // Network 500-599
    EXPECT_EQ(static_cast<uint16_t>(sc::ErrorCode::KVSConnectionFailed), 500);
    // Resource 600-699
    EXPECT_EQ(static_cast<uint16_t>(sc::ErrorCode::MemoryLimitExceeded), 600);
    // AI 700-799
    EXPECT_EQ(static_cast<uint16_t>(sc::ErrorCode::AIModelTimeout), 700);
}

// ============================================================
// Result<T> — Ok/Err construction, is_ok/is_err, value/error
// ============================================================

TEST(ResultInt, OkConstruction) {
    auto r = sc::Result<int>::Ok(42);
    EXPECT_TRUE(r.is_ok());
    EXPECT_FALSE(r.is_err());
    EXPECT_EQ(r.value(), 42);
}

TEST(ResultInt, ErrConstruction) {
    auto r = sc::Result<int>::Err(
        sc::Error{sc::ErrorCode::InvalidArgument, "bad arg", "test"});
    EXPECT_TRUE(r.is_err());
    EXPECT_FALSE(r.is_ok());
    EXPECT_EQ(r.error().code, sc::ErrorCode::InvalidArgument);
    EXPECT_EQ(r.error().message, "bad arg");
    EXPECT_EQ(r.error().context, "test");
}

TEST(ResultString, OkConstruction) {
    auto r = sc::Result<std::string>::Ok("hello");
    EXPECT_TRUE(r.is_ok());
    EXPECT_EQ(r.value(), "hello");
}

TEST(ResultString, ErrConstruction) {
    auto r = sc::Result<std::string>::Err(
        sc::Error{sc::ErrorCode::NotFound, "missing", ""});
    EXPECT_TRUE(r.is_err());
    EXPECT_EQ(r.error().code, sc::ErrorCode::NotFound);
}

// ============================================================
// Result<T> — and_then chaining
// ============================================================

TEST(ResultChain, AndThenOnOk) {
    auto r = sc::Result<int>::Ok(10);
    auto r2 = r.and_then([](int v) {
        return sc::Result<std::string>::Ok(std::to_string(v * 2));
    });
    EXPECT_TRUE(r2.is_ok());
    EXPECT_EQ(r2.value(), "20");
}

TEST(ResultChain, AndThenOnErr) {
    auto r = sc::Result<int>::Err(
        sc::Error{sc::ErrorCode::Timeout, "timed out", ""});
    bool called = false;
    auto r2 = r.and_then([&called](int v) {
        called = true;
        return sc::Result<std::string>::Ok(std::to_string(v));
    });
    EXPECT_FALSE(called);
    EXPECT_TRUE(r2.is_err());
    EXPECT_EQ(r2.error().code, sc::ErrorCode::Timeout);
    EXPECT_EQ(r2.error().message, "timed out");
}

TEST(ResultChain, AndThenMultipleSteps) {
    auto r = sc::Result<int>::Ok(5);
    auto r2 = r
        .and_then([](int v) { return sc::Result<int>::Ok(v + 10); })
        .and_then([](int v) { return sc::Result<int>::Ok(v * 2); });
    EXPECT_TRUE(r2.is_ok());
    EXPECT_EQ(r2.value(), 30);
}

TEST(ResultChain, AndThenErrorPropagation) {
    // Error at step 1 should skip step 2
    auto r = sc::Result<int>::Ok(5)
        .and_then([](int) -> sc::Result<int> {
            return sc::Result<int>::Err(
                sc::Error{sc::ErrorCode::Unknown, "fail", ""});
        })
        .and_then([](int v) { return sc::Result<int>::Ok(v * 100); });
    EXPECT_TRUE(r.is_err());
    EXPECT_EQ(r.error().code, sc::ErrorCode::Unknown);
}

// ============================================================
// Result<T> — map chaining
// ============================================================

TEST(ResultChain, MapOnOk) {
    auto r = sc::Result<int>::Ok(7);
    auto r2 = r.map([](int v) { return v * 3; });
    EXPECT_TRUE(r2.is_ok());
    EXPECT_EQ(r2.value(), 21);
}

TEST(ResultChain, MapOnErr) {
    auto r = sc::Result<int>::Err(
        sc::Error{sc::ErrorCode::NotFound, "nope", "ctx"});
    bool called = false;
    auto r2 = r.map([&called](int v) {
        called = true;
        return v * 3;
    });
    EXPECT_FALSE(called);
    EXPECT_TRUE(r2.is_err());
    EXPECT_EQ(r2.error().code, sc::ErrorCode::NotFound);
    EXPECT_EQ(r2.error().context, "ctx");
}

TEST(ResultChain, MapTypeConversion) {
    auto r = sc::Result<int>::Ok(42);
    auto r2 = r.map([](int v) { return std::to_string(v); });
    EXPECT_TRUE(r2.is_ok());
    EXPECT_EQ(r2.value(), "42");
}

TEST(ResultChain, MapAndThenCombined) {
    auto r = sc::Result<int>::Ok(3)
        .map([](int v) { return v + 1; })                          // 4
        .and_then([](int v) { return sc::Result<int>::Ok(v * 10); }) // 40
        .map([](int v) { return std::to_string(v); });              // "40"
    EXPECT_TRUE(r.is_ok());
    EXPECT_EQ(r.value(), "40");
}

// ============================================================
// VoidResult specialization
// ============================================================

TEST(VoidResult, OkVoid) {
    sc::VoidResult r = sc::OkVoid();
    EXPECT_TRUE(r.is_ok());
    EXPECT_FALSE(r.is_err());
}

TEST(VoidResult, ErrVoid) {
    sc::VoidResult r = sc::ErrVoid(
        sc::ErrorCode::ConfigParseError, "parse failed", "line 42");
    EXPECT_TRUE(r.is_err());
    EXPECT_FALSE(r.is_ok());
    EXPECT_EQ(r.error().code, sc::ErrorCode::ConfigParseError);
    EXPECT_EQ(r.error().message, "parse failed");
    EXPECT_EQ(r.error().context, "line 42");
}

TEST(VoidResult, ErrVoidDefaultContext) {
    sc::VoidResult r = sc::ErrVoid(sc::ErrorCode::Unknown, "oops");
    EXPECT_TRUE(r.is_err());
    EXPECT_EQ(r.error().context, "");
}

TEST(VoidResult, AndThenOnOk) {
    sc::VoidResult r = sc::OkVoid();
    auto r2 = r.and_then([](std::monostate) {
        return sc::Result<int>::Ok(99);
    });
    EXPECT_TRUE(r2.is_ok());
    EXPECT_EQ(r2.value(), 99);
}

TEST(VoidResult, AndThenOnErr) {
    sc::VoidResult r = sc::ErrVoid(sc::ErrorCode::Timeout, "slow");
    auto r2 = r.and_then([](std::monostate) {
        return sc::Result<int>::Ok(99);
    });
    EXPECT_TRUE(r2.is_err());
    EXPECT_EQ(r2.error().code, sc::ErrorCode::Timeout);
}

TEST(VoidResult, MapOnOk) {
    sc::VoidResult r = sc::OkVoid();
    auto r2 = r.map([](std::monostate) { return 123; });
    EXPECT_TRUE(r2.is_ok());
    EXPECT_EQ(r2.value(), 123);
}

TEST(VoidResult, MapOnErr) {
    sc::VoidResult r = sc::ErrVoid(sc::ErrorCode::Unknown, "err");
    auto r2 = r.map([](std::monostate) { return 123; });
    EXPECT_TRUE(r2.is_err());
}


// ============================================================
// Result<T> — move semantics (rvalue value() overload)
// ============================================================

TEST(ResultMove, MoveValueOut) {
    auto r = sc::Result<std::string>::Ok("moveable");
    std::string moved = std::move(r).value();
    EXPECT_EQ(moved, "moveable");
}

// ============================================================
// Result<T> — mutable access
// ============================================================

TEST(ResultMutable, MutableValue) {
    auto r = sc::Result<int>::Ok(10);
    r.value() = 20;
    EXPECT_EQ(r.value(), 20);
}

TEST(ResultMutable, MutableError) {
    auto r = sc::Result<int>::Err(
        sc::Error{sc::ErrorCode::Unknown, "original", ""});
    r.error().message = "modified";
    EXPECT_EQ(r.error().message, "modified");
}

// ============================================================
// Result<T> — accessing wrong variant throws
// ============================================================

TEST(ResultAccess, ValueOnErrThrows) {
    auto r = sc::Result<int>::Err(
        sc::Error{sc::ErrorCode::Unknown, "err", ""});
    EXPECT_THROW(r.value(), std::bad_variant_access);
}

TEST(ResultAccess, ErrorOnOkThrows) {
    auto r = sc::Result<int>::Ok(42);
    EXPECT_THROW(r.error(), std::bad_variant_access);
}

// ============================================================
// Error struct
// ============================================================

TEST(ErrorStruct, EmptyContext) {
    sc::Error e{sc::ErrorCode::OK, "msg", ""};
    EXPECT_EQ(e.code, sc::ErrorCode::OK);
    EXPECT_EQ(e.message, "msg");
    EXPECT_TRUE(e.context.empty());
}
