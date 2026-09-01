# SPDX-License-Identifier: GPL-3.0-or-later
#
# Defines the interface target torquebus::warnings, linked privately by every
# first-party target so that third-party code fetched into the build tree keeps
# its own (usually laxer) warning configuration.

add_library(torquebus_warnings INTERFACE)
add_library(torquebus::warnings ALIAS torquebus_warnings)

if(NOT TORQUEBUS_ENABLE_WARNINGS)
    return()
endif()

if(MSVC)
    target_compile_options(torquebus_warnings INTERFACE
        /W4
        /permissive-
        /Zc:__cplusplus
        /Zc:preprocessor
        /utf-8
        /w14242  # conversion, possible loss of data
        /w14263  # member function does not override any base class virtual
        /w14265  # class has virtual functions but destructor is not virtual
        /w14287  # unsigned/negative constant mismatch
        /w14296  # expression is always false
        /w14545  # expression before comma evaluates to a function missing args
        /w14619  # pragma warning: there is no warning number
        /w14640  # thread-unsafe static member initialization
        /w14826  # conversion is sign-extended
        /w14905  # wide string literal cast to LPSTR
        /w14906  # string literal cast to LPWSTR
        /w14928  # illegal copy-initialization
    )
    if(TORQUEBUS_WARNINGS_AS_ERRORS)
        target_compile_options(torquebus_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(torquebus_warnings INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wcast-align
        -Wunused
        -Woverloaded-virtual
        -Wconversion
        -Wsign-conversion
        -Wdouble-promotion
        -Wformat=2
    )
    if(TORQUEBUS_WARNINGS_AS_ERRORS)
        target_compile_options(torquebus_warnings INTERFACE -Werror)
    endif()
endif()
