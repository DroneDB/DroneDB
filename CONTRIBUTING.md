# Contributing

:tada: First off, thanks for considering being a contributor! :tada:

## Comment Style Conventions

DroneDB uses a consistent commenting style across the C++ codebase. Please follow these conventions:

### License Header

Every file in `src/`, `apps/`, `cmd/`, and `tests/` must start with the MPL 2.0 license header:

```cpp
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
```

Files in `vendor/` are excluded (they retain their own licenses).

### API Documentation (Headers)

Use **Doxygen `/** ... */`** (Javadoc-style) blocks for public API functions, classes, structs, and enums in header files:

```cpp
/**
 * @brief Short one-line description
 *
 * Longer description if needed.
 *
 * @param name Description of the parameter
 * @return Description of the return value
 * @throws ExceptionType When and why it throws
 */
```

For single-line descriptions, a compact form is acceptable:

```cpp
/** Get library version */
```

### Inline Comments (Source Files)

Use `//` single-line comments for inline explanations within `.cpp` files:

```cpp
// Forward declaration for stale lock detection helper
// Returns true if the given file exists and has non-zero size.
```

Short end-of-line notes may use C-style `/* ... */`:

```cpp
y = fabs(y); /* The result doesn't depend on the sign of y */
```

### Section Dividers

Use `// ───` for section separators in headers and source files:

```cpp
// ─── Internal types ─────────────────────────────────────────────────────────
// ─── Public API ─────────────────────────────────────────────────────────────
```

Avoid decorative characters like `═══` or `§` in section dividers.

### What NOT to Use

- **`///`** (triple-slash): Do not use. Convert to `/** ... */` instead.
- **`//!`** (exclamation): Do not use.
- **MSDN XML style** (`<summary>`, `<param name="">`): Not recognized by Doxygen. Use `@brief`, `@param` instead.
- **Multi-line `/* ... */`** for API docs: Use `/** ... */` with Doxygen tags instead.

## How can I contribute?

DDB contributors are expected to follow the [Collective Code of Construction Contract (C4)](https://rfc.zeromq.org/spec:42/C4/). You should read the document before making a pull request.

Take a look at the list of [open issues](https://github.com/uav4geo/ddb/issues) to find out how you can help.

## Code of Conduct

This project adheres to the [Contributor Covenant](CONDUCT.md). By participating, you are expected to uphold this code. Please report unacceptable behavior to the project maintainers.

