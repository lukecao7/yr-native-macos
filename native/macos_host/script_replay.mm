// SPDX-License-Identifier: GPL-3.0-only
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>

#include "frame_image.h"
#include "script_replay.h"

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace coastal::macos_host {
namespace {

namespace fs = std::filesystem;

class FileDescriptor {
public:
    explicit FileDescriptor(int fd) : fd_(fd) {}
    ~FileDescriptor() { close(); }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    int get() const noexcept { return fd_; }
    int release() noexcept { return std::exchange(fd_, -1); }
    int close() noexcept { return fd_ < 0 ? 0 : ::close(release()); }
    void reset(int fd) noexcept { close(); fd_ = fd; }

private:
    int fd_;
};

std::system_error file_error(std::string_view message, int code = errno) {
    return {code, std::generic_category(), std::string(message)};
}

NSData* bytes(std::string_view text) {
    return [NSData dataWithBytes:text.data() length:text.size()];
}

std::string utf8(NSString* value) {
    NSData* data = [value dataUsingEncoding:NSUTF8StringEncoding];
    if (data.length == 0) return {};
    return {static_cast<const char*>(data.bytes), data.length};
}

bool is_boolean(id value) {
    return [value isKindOfClass:NSNumber.class] &&
        CFGetTypeID((__bridge CFTypeRef)value) == CFBooleanGetTypeID();
}

[[noreturn]] void invalid_field(NSString* name, std::string_view expected) {
    throw std::invalid_argument("Field '" + utf8(name) + "' must be " + std::string(expected) + ".");
}

std::string text_field(NSDictionary* request, NSString* name, bool optional = false) {
    id value = request[name];
    if (!value && optional) return {};
    if (![value isKindOfClass:NSString.class]) invalid_field(name, "a string");
    return utf8(value);
}

std::uint64_t unsigned_field(NSDictionary* request, NSString* name,
                             std::optional<std::uint64_t> fallback = std::nullopt,
                             std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max()) {
    id value = request[name];
    if (!value && fallback) return *fallback;
    if ([value isKindOfClass:NSNumber.class] && !is_boolean(value)) {
        const auto text = utf8([value stringValue]);
        std::uint64_t number = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
        if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && number <= maximum)
            return number;
    }
    invalid_field(name, "an unsigned integer in range");
}

double coordinate(NSDictionary* request, NSString* name) {
    id value = request[name];
    if ([value isKindOfClass:NSNumber.class] && !is_boolean(value)) {
        const auto number = [value doubleValue];
        if (std::isfinite(number)) return number;
    }
    invalid_field(name, "a finite pixel coordinate");
}

bool boolean_field(NSDictionary* request, NSString* name) {
    id value = request[name];
    if (!value) return false;
    if (!is_boolean(value)) invalid_field(name, "a boolean");
    return [value boolValue] != NO;
}

PointerEvent::Phase pointer_phase(const std::string& name) {
    using Phase = PointerEvent::Phase;
    if (name == "down") return Phase::Down;
    if (name == "drag") return Phase::Drag;
    if (name == "up") return Phase::Up;
    if (name == "move") return Phase::Move;
    if (name == "enter") return Phase::Enter;
    if (name == "leave") return Phase::Leave;
    invalid_field(@"phase", "down, drag, up, move, enter or leave");
}

KeyEvent::Phase key_phase(const std::string& name) {
    using Phase = KeyEvent::Phase;
    if (name == "down") return Phase::Down;
    if (name == "up") return Phase::Up;
    if (name == "modifiersChanged") return Phase::ModifiersChanged;
    invalid_field(@"phase", "down, up or modifiersChanged");
}

fs::path capture_path(NSDictionary* request) {
    const auto name = text_field(request, @"capture");
    const fs::path path(name);
    if (name.find('\0') != std::string::npos || path.is_absolute() || path.extension() != ".png")
        invalid_field(@"capture", "a relative .png path");
    for (const auto& part : path) {
        if (part == ".." || part == ".") invalid_field(@"capture", "a path below the output directory");
    }
    return path;
}

constexpr int directory_flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_DIRECTORY;

void write_capture(int output, const fs::path& relative, NSData* png) {
    FileDescriptor parent(::openat(output, ".", directory_flags));
    if (parent.get() < 0) throw file_error("Open capture output directory");
    for (const auto& part : relative.parent_path()) {
        if (::mkdirat(parent.get(), part.c_str(), 0700) != 0 && errno != EEXIST)
            throw file_error("Create capture directory");
        const int child = ::openat(parent.get(), part.c_str(), directory_flags);
        if (child < 0) throw file_error("Open capture directory");
        parent.reset(child);
    }
    const auto filename = relative.filename();

    const auto temp = ".coastal-capture-" + utf8(NSUUID.UUID.UUIDString);
    FileDescriptor file(::openat(parent.get(), temp.c_str(),
                                 O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (file.get() < 0) throw file_error("Create temporary PNG");
    try {
        auto* cursor = static_cast<const std::uint8_t*>(png.bytes);
        auto remaining = png.length;
        while (remaining) {
            const auto written = ::write(file.get(), cursor, remaining);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) throw file_error("Write capture PNG", written == 0 ? EIO : errno);
            cursor += written;
            remaining -= static_cast<NSUInteger>(written);
        }
        if (file.close() != 0) throw file_error("Close capture PNG");
        if (::linkat(parent.get(), temp.c_str(), parent.get(), filename.c_str(), 0) != 0)
            throw file_error("Commit capture PNG");
    } catch (...) {
        ::unlinkat(parent.get(), temp.c_str(), 0);
        throw;
    }
    ::unlinkat(parent.get(), temp.c_str(), 0);
}

NSData* encode_png(Viewport viewport, const Frame& frame) {
    const auto space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    if (!space) throw std::runtime_error("Cannot create the capture color space.");
    const auto image = create_rgba_frame_image(viewport, frame, space);
    CGColorSpaceRelease(space);
    if (!image) throw std::runtime_error("Cannot create the capture image.");
    NSMutableData* data = [NSMutableData data];
    const auto destination = CGImageDestinationCreateWithData(
        (__bridge CFMutableDataRef)data, CFSTR("public.png"), 1, nullptr);
    bool encoded = false;
    if (destination) {
        CGImageDestinationAddImage(destination, image, nullptr);
        encoded = CGImageDestinationFinalize(destination);
        CFRelease(destination);
    }
    CGImageRelease(image);
    if (!encoded) throw std::runtime_error("Cannot encode the capture as PNG.");
    return data;
}

class Replay {
public:
    Replay(Session& session, int output)
        : session_(session), output_(output), viewport_(session.viewport()),
          pixel_count_(std::size_t(viewport_.width) * viewport_.height * 4) {}

    int run(std::FILE* input) {
        // Reply as soon as each line arrives so an interactive stdin writer can advance.
        std::size_t length = 0;
        while (const char* line = ::fgetln(input, &length)) {
            @autoreleasepool {
                if (process({line, length})) return failed_ ? 1 : 0;
            }
        }
        if (std::ferror(input)) throw std::runtime_error("Cannot read the replay script.");
        return failed_ ? 1 : 0;
    }

private:
    bool process(std::string_view line) {
        id parsed = [NSJSONSerialization JSONObjectWithData:bytes(line) options:0 error:nil];
        NSDictionary* request = [parsed isKindOfClass:NSDictionary.class] ? parsed : nil;
        id request_id = request[@"id"];
        id op = request[@"op"];
        NSMutableDictionary* reply = [@{ @"id": request_id ? request_id : NSNull.null,
                                         @"op": op ? op : NSNull.null, @"ok": @YES } mutableCopy];
        const auto before = frame_count_;
        bool quit = false;
        try {
            if (!request) throw std::invalid_argument("Expected one JSON object per line.");
            quit = execute(request, text_field(request, @"op"), reply);
        } catch (const std::exception& e) {
            reply[@"ok"] = @NO;
            NSString* message = [NSString stringWithUTF8String:e.what()];
            reply[@"error"] = message ? message : @"Replay command failed.";
            failed_ = true;
        }
        if ([op isEqual:@"frame"]) reply[@"advanced"] = @(frame_count_ - before);
        reply[@"frameIndex"] = frame_count_ ? @(frame_count_ - 1) : NSNull.null;
        NSData* data = [NSJSONSerialization dataWithJSONObject:reply options:NSJSONWritingSortedKeys error:nil];
        if (!data) throw std::runtime_error("Cannot encode the replay reply.");
        std::cout << "COASTAL_SCRIPT " << std::string_view(static_cast<const char*>(data.bytes), data.length)
                  << std::endl;
        if (!std::cout) throw std::runtime_error("Cannot write the replay reply.");
        return quit;
    }

    bool execute(NSDictionary* request, const std::string& op, NSMutableDictionary* reply) {
        if (op == "frame") {
            const auto count = unsigned_field(request, @"count", std::nullopt,
                600);
            if (!count) invalid_field(@"count", "a positive integer");
            const auto path = request[@"capture"] ? std::optional(capture_path(request)) : std::nullopt;
            for (std::uint64_t i = 0; i < count; ++i) advance();
            if (path) capture(*path, request[@"capture"], reply);
        } else if (op == "capture") {
            capture(capture_path(request), request[@"capture"], reply);
        } else if (op == "pointer") {
            const auto phase = pointer_phase(text_field(request, @"phase"));
            using Phase = PointerEvent::Phase;
            const bool clicking = phase == Phase::Down || phase == Phase::Drag || phase == Phase::Up;
            constexpr auto max_pointer_value = std::numeric_limits<std::uint32_t>::max();
            session_.pointer_event({phase,
                static_cast<std::uint32_t>(unsigned_field(request, @"button", 0, max_pointer_value)),
                coordinate(request, @"x"), coordinate(request, @"y"),
                unsigned_field(request, @"modifiers", 0),
                static_cast<std::uint32_t>(unsigned_field(request, @"clickCount", clicking ? 1 : 0, max_pointer_value))});
        } else if (op == "key") {
            const auto chars = text_field(request, @"chars", true);
            const auto plain = request[@"charsIgnoringModifiers"] ? text_field(request, @"charsIgnoringModifiers") : chars;
            session_.key_event({key_phase(text_field(request, @"phase")),
                static_cast<std::uint16_t>(unsigned_field(request, @"code", std::nullopt,
                    std::numeric_limits<std::uint16_t>::max())),
                unsigned_field(request, @"modifiers", 0), boolean_field(request, @"repeat"), chars, plain});
        } else if (op == "state") {
            reply[@"viewportWidth"] = @(viewport_.width);
            reply[@"viewportHeight"] = @(viewport_.height);
        } else if (op == "quit") {
            return true;
        } else {
            throw std::invalid_argument("Unknown replay operation.");
        }
        return false;
    }

    void advance() {
        latest_.reset();
        auto frame = session_.advance_frame();
        if (!frame || frame->scene || frame->rgba.size() != pixel_count_)
            throw std::runtime_error("The session did not return a complete 2D raster.");
        if (frame->commit.index != frame_count_)
            throw std::runtime_error("The session reported a nonconsecutive frame.");
        latest_ = std::move(*frame);
        ++frame_count_;
    }

    void capture(const fs::path& path, NSString* name, NSMutableDictionary* reply) const {
        if (!latest_) throw std::runtime_error("Advance at least one frame before capturing.");
        write_capture(output_, path, encode_png(viewport_, *latest_));
        reply[@"capture"] = name;
    }

    Session& session_;
    const int output_;
    const Viewport viewport_;
    const std::size_t pixel_count_;
    std::optional<Frame> latest_;
    std::uint64_t frame_count_ = 0;
    bool failed_ = false;
};

}  // namespace

int replay_script(const LaunchOptions& options, Session& session) {
    std::unique_ptr<std::FILE, decltype(&std::fclose)> script(nullptr, &std::fclose);
    if (options.script_file != "-") {
        FileDescriptor input(::open(options.script_file.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
        if (input.get() < 0) throw file_error("Open --script-file");
        struct stat status{};
        if (::fstat(input.get(), &status) != 0) throw file_error("Inspect --script-file");
        if (!S_ISREG(status.st_mode)) throw std::invalid_argument("--script-file requires a regular file or stdin (-).");
        script.reset(::fdopen(input.get(), "r"));
        if (!script) throw file_error("Open --script-file as a stream");
        input.release();
    }
    if (::mkdir(options.script_output.c_str(), 0700) != 0)
        throw file_error("Create new --script-output directory");
    FileDescriptor output(::open(options.script_output.c_str(), directory_flags));
    if (output.get() < 0) throw file_error("Open --script-output directory");
    Replay replay(session, output.get());
    return replay.run(script ? script.get() : stdin);
}

}  // namespace coastal::macos_host
