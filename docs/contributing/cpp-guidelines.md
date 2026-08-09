<!--
SPDX-FileCopyrightText: 2026 Théo Magne

SPDX-License-Identifier: MIT
-->

# C++ coding guidelines <!-- omit in toc -->

Generic, project-agnostic C++ conventions. These are language-level rules meant to be reusable
across C++ projects; they make no reference to any single codebase. Project-specific conventions
(component layout, DI, frameworks, FFI boundaries) belong in a per-project document that links here.

This guide does **not** restate what a formatter and linter enforce. Each project's
`.clang-format` and `.clang-tidy` are the source of truth for formatting and identifier casing -
configure them to match the conventions below and let CI enforce them.

## Project addendum: drone-mark4 <!-- omit in toc -->

This repo applies the guide with overrides driven by the hard rules of the root `CLAUDE.md`
(no exceptions/RTTI and no `new`/`delete` in flight-core and platform, no iostream in
flight-core, no singletons anywhere):

- **Error handling**: the exception guidance does not apply where `-fno-exceptions` rules
  (flight-core, platform, every app linking `drone_strict`). Report failures with `bool`
  returns and let the failing service log, as the App `init()` pattern does.
- **Ownership and lifetime / factory and clone patterns**: `std::unique_ptr` factories and
  `cloneUniquePtr()` rely on dynamic allocation. Here, services live as value members of an
  App composition root and dependencies are injected by reference.
- **`equals()` vs `operator==`**: the `dynamic_cast` based pattern needs RTTI, disabled in
  flight-core and platform.

Everything else applies as written; the root `.clang-format` / `.clang-tidy` are the enforced
source of truth.

## Table of content <!-- omit in toc -->

- [Naming](#naming)
- [Enumerations](#enumerations)
- [Namespaces](#namespaces)
- [File and component layout](#file-and-component-layout)
- [Ownership and lifetime](#ownership-and-lifetime)
- [Error handling](#error-handling)
  - [Returning a value alongside a success flag](#returning-a-value-alongside-a-success-flag)
  - [Discriminating failure reasons](#discriminating-failure-reasons)
- [Logging](#logging)
- [Polymorphism](#polymorphism)
  - [Diamond inheritance](#diamond-inheritance)
  - [Multiple inheritance as capability mixins](#multiple-inheritance-as-capability-mixins)
  - [`equals()` vs `operator==`](#equals-vs-operator)
  - [Clone vs copy constructor](#clone-vs-copy-constructor)
- [Modern C++ defaults](#modern-c-defaults)
- [Documentation](#documentation)
- [Sources](#sources)

## Naming

Those elements are enforced by the linter; the examples are canonical, not exhaustive.

| Element                      | Convention           | Example                          |
| ---------------------------- | -------------------- | -------------------------------- |
| Files                        | `snake_case`         | `led_service.hpp`                |
| Classes / structs            | `PascalCase`         | `SessionManager`                 |
| Abstract base classes        | `Abs` prefix           | `AbsLedService`, `AbsPlatform`       |
| Instance methods             | `camelCase`          | `setColor()`, `postEvent()`      |
| Static methods               | `PascalCase`         | `Current()`, `FromConfig()`      |
| Free functions               | `camelCase`          | `createPlatform()`, `toString()` |
| Member variables             | `m_` + `camelCase`   | `m_logger`                       |
| Static member variables      | `s_` + `camelCase`   | `s_instance`                     |
| Constants / `constexpr`      | `UPPER_SNAKE`        | `DEFAULT_TIMEOUT_MS`             |
| Enum type                    | `PascalCase`         | `LedColor`                       |
| Enumerators                  | `UPPER_SNAKE`        | `LedColor::GREEN`                |
| Factory functions            | `createXxx()`        | `createPlatform()`               |
| Local variables / parameters | `camelCase`          | `index`, `sensorValue`           |
| Namespace names              | `camelCase`          | `myproj`, `audio`                |
| Template parameters          | `T` or `TPascalCase` | `T`, `TValue`, `TCallback`       |

Use the `Abs` prefix consistently for abstract bases; do not mix in alternative prefixes (`A`, `I`,
`Base`) for the same role.

**Static methods take `PascalCase`, not `camelCase`** - they are type-level operations invoked as
`Type::Method()`, named distinctly from instance methods (which act on an object and use
`camelCase`). Factory *free functions* keep `createXxx()` (`camelCase`); a factory exposed as a
static **method** follows the static-method rule and is `PascalCase`.

## Enumerations

Use a **bare scoped enum** (`enum class`). Do **not** wrap it in a namespace.

```cpp
enum class LedColor
{
    OFF,
    GREEN,
    RED,
};
// used as: LedColor::GREEN
```

A scoped enum already namespaces its enumerators, so an enclosing namespace
(`namespace LedColor { enum class Enum { ... } }`) is redundant and only makes call sites longer
(`LedColor::Enum::GREEN` versus `LedColor::GREEN`). This follows the C++ Core Guidelines (Enum.3,
"prefer `enum class`").

- When you need helpers (e.g. `toString`), add **free functions** in the same namespace as the
  enum rather than wrapping the enum.

  ```cpp
  namespace myproj::audio
  {
      enum class LedColor { OFF, GREEN, RED };
      std::string_view toString(LedColor color);   // overload #1

      enum class LedState { ACTIVE, IDLE };
      std::string_view toString(LedState state);   // overload #2 - no conflict
  }
  ```

- To drop the prefix inside a `switch`, use C++20 `using enum LedColor;` locally.
- When an enum doubles as an error code, `OK` must be the first enumerator (value `0`):
  zero-initialization then yields a valid state.

## Namespaces

A namespace has one primary job: **preventing name collisions** between independently-developed
compilation units. It is not an organizational hierarchy and must not mirror the directory tree.

Keep namespaces shallow and tied to large-scale structure (project, then optionally one component
level). **Do not mirror the directory tree in the namespace.** A rule of thumb is **three levels or
fewer**.

```cpp
namespace myproj::audio { /* ... */ }   // good
namespace myproj::audio::codecs::mp3::detail::v2 { /* ... */ }  // too deep
```

This is not merely aesthetic:

- **Name lookup.** C++ resolves an unqualified name by searching enclosing namespaces outward, so
  code in a deeply nested namespace can unintentionally bind a symbol from a parent namespace - and
  a transitive include that introduces a matching leading namespace can break the build. Deep
  nesting **exacerbates** collision risk rather than shielding from it.
- **Maintainability.** Files move between directories as a design evolves; updating namespaces to
  match is cumbersome, and a single translation unit already holds several namespaces (`detail`,
  anonymous), so a 1:1 directory-to-namespace mapping cannot hold.
- **Readability.** Each extra level lengthens every fully-qualified name or forces more `using`
  declarations.

Treat namespaces as large-scale organization and folders as everything finer - the two depths are
independent. Use the collapsed C++17 form: `namespace myproj::audio { }`.

**Choosing the right depth.** The following patterns capture the canonical decisions:

```text
myproj                <- single C++ deliverable; project name is the collision anchor
myproj::audio         <- add this level only when "audio" ships as its own independently-
                         distributed library, versioned separately from the project root
myproj::audio::codec  <- anti-pattern: third level mirrors a subfolder, not a distinct
                         shipped unit
```

```cpp
// Good - one project, one root namespace; folder structure carries all sub-organisation
namespace myproj { class AudioCodec { /* ... */ }; }

// Good - two independently-distributed libraries under the same project
namespace myproj::audio   { /* ... */ }
namespace myproj::network { /* ... */ }

// Anti-pattern - "codec" is a folder inside audio/, not an independent library
namespace myproj::audio::codec { /* ... */ }  // mirrors a directory; buys nothing

// Surgical - sub-namespace to resolve a real local name collision
namespace myproj::audio
{
    namespace events   // State names would clash across domains without this scope
    {
        enum class State { IDLE, PLAYING, PAUSED };
    }

    namespace detail { /* internal helpers, not part of the public API */ }
}
```

**Utility sub-namespaces.** When a `utilities/` file groups related free functions, one additional
leaf sub-namespace below the component namespace is allowed - and preferred over a utility class
with only static methods:

```cpp
namespace myproj::audio
{
    namespace FormatUtils   // leaf grouping - one extra level is acceptable
    {
        [[nodiscard]] Buffer normalize(const Buffer &buf);
    }
} // namespace myproj::audio
```

This is not a violation of the three-level rule: it does not mirror the directory tree and lives
within a single header/source pair. The call site (`FormatUtils::normalize(buf)`) is identical to a
static-method call, while retaining the advantages of a real namespace: ADL-compatible,
reopenable, and no accidental instantiation.

**Never use a class with only static methods as a namespace substitute.** Such a class offers no
advantage over a namespace but adds pitfalls: it can be instantiated (unless the default
constructor is explicitly deleted), it blocks ADL, and it cannot be extended by free functions in
the same namespace.

## File and component layout

- Header/source split; **one public class per file**; `snake_case` file names.
- Public headers under `include/<component>/`, implementation (and private headers) under `src/`.
- Group files into subfolders by concern. Canonical names - reuse these rather than inventing
  synonyms:

```text
<component>/
|-- include/<component>/
|   |-- constants/      # compile-time constants, grouped by topic
|   |-- models/         # data / value types
|   |-- services/       # behaviour, managers, workers
|   |-- types/          # enums and small POD types
|   `-- utilities/      # free-function helpers
`-- src/                # mirrors include/
```

Smaller components may omit unused folders. Folder depth is independent of namespace depth (see
[Namespaces](#namespaces)).

## Ownership and lifetime

Express ownership in the type system; avoid manual lifetime management.

- **Owned, exclusive sub-objects:** `std::unique_ptr`.
- **Non-owning dependencies** injected at construction: store a **reference** (`T&`).
- **Shared ownership:** `std::shared_ptr`, reserved for genuinely shared resources (e.g. loggers).
- **Avoid raw `new`/`delete`** and owning raw pointers in ordinary code; prefer RAII.

When a member is a `std::unique_ptr` to a **forward-declared** type, the owner's destructor cannot
be defaulted in the header (the type is incomplete there). Declare it in the header and define it in
the `.cpp` where the type is complete, with a short rationale:

```cpp
/* <Type> is forward declared, so the destructor cannot be defaulted in the header; it is defined
   here where the type is complete. */
// NOLINTNEXTLINE(modernize-use-equals-default)
Owner::~Owner() {}
```

## Error handling

- Operations that can fail return **`bool`** (`true` on success) and **log the reason** on failure.
- Reserve **exceptions** for programming errors / unrecoverable invariant violations (e.g. using a
  singleton before it is created). Avoid a custom exception hierarchy; keep it simple.
- Across a **C / FFI boundary**, never throw - return error codes only.
- Use `std::optional` for "value may be absent"; do not use it as an error channel.

### Returning a value alongside a success flag

When an operation can fail **and** must produce a value, use a `bool` return combined with a
`T& out` parameter. Intent is explicit; the caller controls storage and lifetime. The output
parameter's state on failure is unspecified; document it.

```cpp
bool readSensor(float& valueOut);
// valueOut is valid only when the function returns true
```

### Discriminating failure reasons

When the caller needs to distinguish *why* an operation failed, replace the `bool` return with a
scoped enum that includes an `OK` enumerator:

```cpp
enum class InitError
{
    OK,                   ///<
    HARDWARE_UNAVAILABLE, ///<
    TIMEOUT,              ///<
};
```

The logging rule still applies: always log the reason before returning a non-`OK` code. Use enum
error codes only when the call site actually branches on the failure kind; a plain `bool` is
sufficient for simple pass/fail operations.

## Logging

- Obtain a **categorized** logger per class (a `LOGGER_CATEGORY`-style constant keeps the category
  in one place).
- Build a **logger hierarchy** by passing a parent logger down to sub-components and deriving a
  child category from it (e.g. `app -> audio -> codec`), rather than giving every class a flat
  top-level logger.
- Prefer a stream form when composing a message from several values; a direct form for fixed
  strings.

## Polymorphism

Two patterns; pick the one that fits the role.

Abstract base classes (`Abs`-prefixed) must declare a virtual destructor:
`virtual ~AbsBase() = default;`. Without it, `delete base_ptr;` is undefined behaviour
(C++ Core Guidelines C.35).

1. **Service interface + factory** - the default for behaviour/services. An abstract `Abs`-prefixed
   interface with pure-virtual methods, instantiated through a factory that returns
   `std::unique_ptr`. Use for managers, workers, and hardware/service abstractions.

2. **Polymorphic value type** - for small, copyable parameter objects that share a base and need
   value semantics. Provide a virtual `cloneUniquePtr()` and store as `std::unique_ptr<Base>`.

### Diamond inheritance

The diamond problem arises when two base classes share a common ancestor:

```text
        AbsBase
       /     \
   BLeft    CRight
       \     /
        DFull         <- DFull contains two copies of AbsBase
```

This causes ambiguous member access and data duplication. Virtual inheritance exists as a language
workaround but introduces significant complexity. The recommended approach is to restructure the
hierarchy to avoid the diamond entirely.

### Multiple inheritance as capability mixins

Inheriting from several pure-virtual interfaces is a valid and idiomatic pattern for composing
orthogonal capabilities without sharing implementation:

```cpp
class AudioRecorder : public AbsWorker, public AbsSerializable { /* ... */ };
```

Each interface defines a narrow, cohesive set of virtual methods. The class assembles the
capabilities it needs, and each capability can be tested or replaced independently. This is safe
precisely because pure-virtual interfaces carry no state.

### `equals()` vs `operator==`

Two equality mechanisms exist for different contexts; never define them independently on the same
type.

**`operator==`** - for concrete, non-polymorphic types (leaf or `final` classes, plain value
types). Comparison resolves at compile time against the known static type, which makes it
compatible with STL containers and algorithms. Use `= default` for member-wise equality when
applicable.

```cpp
struct Color final
{
    uint8_t r, g, b;
    bool operator==(const Color&) const = default;
};
```

**Do not** define `operator==` directly on a non-`final` polymorphic base: it sees only the base
subobject and silently ignores derived fields, giving incorrect results for derived types.

**`virtual bool equals(const Base&)`** - for polymorphic value types where comparison must happen
through a base reference or pointer. The override checks that `other` is the same derived type
(via `dynamic_cast`), then compares derived-specific fields. `operator==` and `operator!=` are
defined on the base and delegate to `equals`:

```cpp
class AbsParam
{
public:
    [[nodiscard]] virtual bool equals(const AbsParam& other) const = 0;
    bool operator==(const AbsParam& other) const { return equals(other); }
    bool operator!=(const AbsParam& other) const { return !equals(other); }
};

class VolumeParam final : public AbsParam
{
public:
    [[nodiscard]] bool equals(const AbsParam& other) const override
    {
        const auto* rhs = dynamic_cast<const VolumeParam*>(&other);
        return rhs != nullptr && m_level == rhs->m_level;
    }
private:
    int m_level{0};
};
```

### Clone vs copy constructor

A copy constructor resolves at **compile time** against the static type. Copying through a base
reference or pointer causes **object slicing**: derived fields are silently dropped.

```cpp
std::unique_ptr<AbsParam> a = createParam();   // concrete type unknown here
AbsParam copy = *a;                            // sliced - derived fields are lost
```

`virtual cloneUniquePtr()` dispatches at **runtime** through the vtable, returning a
`std::unique_ptr<Base>` that points to a complete copy of the actual derived object. Use it
whenever the concrete type is unknown at the call site.

```cpp
class AbsParam
{
public:
    [[nodiscard]] virtual std::unique_ptr<AbsParam> cloneUniquePtr() const = 0;
};

class VolumeParam final : public AbsParam
{
public:
    [[nodiscard]] std::unique_ptr<AbsParam> cloneUniquePtr() const override
    {
        return std::make_unique<VolumeParam>(*this);
    }
};

// Correct polymorphic copy - no slicing
std::unique_ptr<AbsParam> clone = a->cloneUniquePtr();
```

Use the copy constructor when the concrete type is known statically; use `cloneUniquePtr()` when
working through a base handle.

## Modern C++ defaults

- Mark every method that does not modify observable state as `const`; prefer `const T&` for input
  parameters.
- `[[nodiscard]]` on getters and const methods that return a value.
- `explicit` on every single-argument constructor.
- `= default` / `= delete` to state the Rule of Five intent.
- In-class member initializers for defaults; `static constexpr` for compile-time constants.
- `std::string_view` for non-owning string parameters/returns.
- `std::variant` + `std::visit` for type-safe dispatch over a closed set.

Adopt newer facilities (`std::expected`, `std::format`, concepts, ranges, coroutines, modules) only
by team decision, not ad hoc - they change the baseline a codebase must support.

## Documentation

- Document every public class and method with Doxygen using `@` notation (not `\`). Open key
  headers with `/// @file` + `/// @brief`.
- Method blocks use `@brief`, `@param` / `@param[out]`, `@return`, `@note` as applicable.
- Enum values and struct members use a **trailing** `///<` comment.
- **Comments must describe the code they sit on.** Never copy a doc block to a similar method
  without updating it - a comment that says "compares color" on a method that compares state is
  worse than none.
- Mark deferred work with `TODO(<username>)`; for placeholder values, say so explicitly.

## Sources

- [C++ Core Guidelines - Enum.3 (prefer `enum class`)](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#enum3-prefer-class-enums-over-plain-enums)
- [learncpp - scoped enumerations and `using enum`](https://www.learncpp.com/cpp-tutorial/scoped-enumerations-enum-classes/)
- [abseil Tip #130 - Namespace Naming](https://abseil.io/tips/130)
- [Arne Mertz - Organizing directories and namespaces](https://arne-mertz.de/2016/06/organizing-directories-namespaces/)
- [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html)
- [Scott Meyers - *Effective C++* (3rd ed.), Item 23: prefer non-member non-friend functions](https://www.aristeia.com/books.html)
- [Herb Sutter & Andrei Alexandrescu - *C++ Coding Standards*, Item 44: prefer writing nonmember nonfriend functions](https://www.gotw.ca/publications/c++cs.htm)
- [C++ Core Guidelines - F.20 (for "out" output values, prefer return values over output parameters)](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#Rf-out)
- [C++ Core Guidelines - F.21 (to return multiple "out" values, prefer returning a struct)](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#Rf-out-multi)
- [C++ Core Guidelines - C.135 (use multiple inheritance to represent multiple distinct interfaces)](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#Rh-mi-interface)