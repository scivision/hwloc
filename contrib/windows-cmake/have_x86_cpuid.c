// using macros from https://stackoverflow.com/a/66249936/7703794 (CC BY-SA 3.0)

int main(void) {
#if defined(__x86_64__) || defined(_M_X64) || defined(i386) || defined(__i386__) || defined(__i386) || defined(_M_IX86)
  return 0;
#else
  return 1;
#endif
}
