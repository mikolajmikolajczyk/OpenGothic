// Force-included into every TU of the PS4 build of OpenGothic (see the OG_PS4_SHIM
// block in this title's CMakeLists.txt).
//
// OpenGothic and ZenKit are C++20; the OpenOrbis SDK ships libc++ 11, whose <concepts>
// is a synopsis-only stub - the header exists, declares nothing, and every
// `std::same_as` in ZenKit is an error. The `concepts` file next to this one is a
// replacement found ahead of the SDK's copy on the include path; this header pulls the
// same definitions into TUs that get them transitively (libc++'s own headers include
// <concepts> without going through the include path we prepend).
//
// This is a SHIM, not a fix. The fix is a newer libc++ in the SDK, which is not ours to
// ship. It defines only what the tree actually names; a concept that appears in a
// future ZenKit is a compile error here and should be added deliberately.
//
// NOTE what is deliberately NOT here any more: the recon shim also redefined
// std::abs(float)/std::abs(double), because with the old include order libc++'s
// float overloads were unreachable and `std::abs` on a float silently truncated through
// `int abs(int)`. cmake/ps4-openorbis.cmake now puts libc++'s include directory ahead of
// the SDK's C headers and asserts the binding with a static_assert at configure time, so
// those overloads come from libc++ - and redefining them here would be a redefinition
// error, not a workaround.
#ifndef _TEMPEST_PS4_CXX20_SHIM
#define _TEMPEST_PS4_CXX20_SHIM
#if defined(__cplusplus) && __cplusplus >= 202002L
#include <type_traits>
namespace std {
template<class T, class U> concept same_as = is_same_v<T,U> && is_same_v<U,T>;
template<class D, class B> concept derived_from =
  is_base_of_v<B,D> && is_convertible_v<const volatile D*, const volatile B*>;
template<class F, class T> concept convertible_to =
  is_convertible_v<F,T> && requires(add_rvalue_reference_t<F> (&f)()) { static_cast<T>(f()); };
template<class T> concept integral = is_integral_v<T>;
template<class T> concept signed_integral = integral<T> && is_signed_v<T>;
template<class T> concept unsigned_integral = integral<T> && !is_signed_v<T>;
template<class T> concept floating_point = is_floating_point_v<T>;
template<class T> concept destructible = is_nothrow_destructible_v<T>;
template<class T, class... A> concept constructible_from = destructible<T> && is_constructible_v<T,A...>;
template<class T> concept default_initializable = constructible_from<T> && requires { T{}; };
template<class T> concept move_constructible = constructible_from<T,T> && convertible_to<T,T>;
template<class T> concept copy_constructible = move_constructible<T> &&
  constructible_from<T,T&> && convertible_to<T&,T> && constructible_from<T,const T&> && convertible_to<const T&,T>;
template<class F, class... A> concept invocable = is_invocable_v<F,A...>;
template<class F, class... A> concept regular_invocable = invocable<F,A...>;
template<class T, class U> concept equality_comparable_with = requires(const T& a, const U& b) { a==b; a!=b; };
template<class T> concept equality_comparable = equality_comparable_with<T,T>;
}
#endif
#endif
