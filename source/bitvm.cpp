#include "BitVM.h"
#include "MicroBitTouchDevelop.h"
#include <cstdlib>
#include <climits>
#include <cmath>
#include <vector>


#define DBG printf
//#define DBG(...)

#define bytecode ((uint16_t*)functionsAndBytecode)

#define getstr(off) ((const char*)&bytecode[off])
#define getbytes(off) ((ImageData*)(void*)&bytecode[off])

// Macros to reference function pointer in the jump-list
// c in mbitc - stands for 'common'
#define mbit(x) (void*)bitvm_micro_bit::x,
#define mbitc(x) (void*)micro_bit::x,

namespace bitvm {

  uint32_t ldloc(RefLocal *r)
  {
    //DBG("LD "); r->print();
    return r->v;
  }

  uint32_t ldlocRef(RefRefLocal *r)
  {
    uint32_t tmp = r->v;
    incr(tmp);
    return tmp;
  }

  void stloc(RefLocal *r, uint32_t v)
  {
    r->v = v;
    //DBG("ST "); r->print();
  }

  void stlocRef(RefRefLocal *r, uint32_t v)
  {
    decr(r->v);
    r->v = v;
  }

  RefLocal *mkloc()
  {
    return new RefLocal();
  }

  RefRefLocal *mklocRef()
  {
    return new RefRefLocal();
  }

  uint32_t ldfld(RefRecord *r, int idx)
  {
    return r->ld(idx);
  }

  uint32_t ldfldRef(RefRecord *r, int idx)
  {
    return r->ldref(idx);
  }

  void stfld(RefRecord *r, int idx, uint32_t val)
  {
    r->st(idx, val);
  }

  void stfldRef(RefRecord *r, int idx, uint32_t val)
  {
    r->stref(idx, val);
  }

  uint32_t ldglb(int idx)
  {
    check(0 <= idx && idx < numGlobals, ERR_OUT_OF_BOUNDS, 7);
    return globals[idx];
  }

  uint32_t ldglbRef(int idx)
  {
    check(0 <= idx && idx < numGlobals, ERR_OUT_OF_BOUNDS, 7);
    uint32_t tmp = globals[idx];
    incr(tmp);
    return tmp;
  }

  uint32_t is_invalid(uint32_t v)
  {
    return v == 0;
  }

  // note the idx comes last - it's more convenient that way in the emitter
  void stglb(uint32_t v, int idx)
  {
    check(0 <= idx && idx < numGlobals, ERR_OUT_OF_BOUNDS, 7);
    globals[idx] = v;
  }

  void stglbRef(uint32_t v, int idx)
  {
    check(0 <= idx && idx < numGlobals, ERR_OUT_OF_BOUNDS, 7);
    decr(globals[idx]);
    globals[idx] = v;
  }

  // Store a captured local in a closure. It returns the action, so it can be chained.
  RefAction *stclo(RefAction *a, int idx, uint32_t v)
  {
    //DBG("STCLO "); a->print(); DBG("@%d = %p\n", idx, (void*)v);
    a->st(idx, v);
    return a;
  }

  // This one is used for testing in 'bitvm test0'
  uint32_t const3() { return 3; }

#ifdef DEBUG_MEMLEAKS
  std::set<RefObject*> allptrs;
  void debugMemLeaks()
  {
    printf("LIVE POINTERS:\n");
    for(std::set<RefObject*>::iterator itr = allptrs.begin();itr!=allptrs.end();itr++)
    {
      (*itr)->print();
    }    
    printf("\n");
  }
#else
  void debugMemLeaks() {}
#endif

  namespace bitvm_number {
    void post_to_wall(int n) { printf("%d\n", n); }

    StringData *to_character(int x)
    {
      return ManagedString((char)x).leakData();
    }

    StringData *to_string(int x)
    {
      return ManagedString(x).leakData();
    }
  }

  namespace contract {
    void assert(int cond, uint32_t msg)
    {
      if (cond == 0) {
        printf("Assertion failed: %s\n", getstr(msg));
        die();
      }
    }
  }

  namespace string {
    StringData *mkEmpty()
    {
      return ManagedString::EmptyString.leakData();
    }

    StringData *concat(StringData *s1, StringData *s2) {
      ManagedString a(s1), b(s2);
      return (a + b).leakData();
    }

    StringData *concat_op(StringData *s1, StringData *s2) {
      return concat(s1, s2);
    }

    StringData *substring(StringData *s, int i, int j) {
      return (ManagedString(s).substring(i, j)).leakData();
    }

    bool equals(StringData *s1, StringData *s2) {
      return ManagedString(s1) == ManagedString(s2);
    }

    int count(StringData *s) {
      return s->len;
    }

    StringData *at(StringData *s, int i) {
      return ManagedString((char)ManagedString(s).charAt(i)).leakData();
    }

    int to_character_code(StringData *s) {
      return ManagedString(s).charAt(0);
    }

    int code_at(StringData *s, int i) {
      return ManagedString(s).charAt(i);
    }

    int to_number(StringData *s) {
      return atoi(s->data);
    }

    void post_to_wall(StringData *s) { printf("%s\n", s->data); }
  }

  namespace bitvm_boolean {
    // Cache the string literals "true" and "false" when used.
    // Note that the representation of booleans stays the usual C-one.
    
    static const char sTrue[]  __attribute__ ((aligned (4))) = "\xff\xff\x04\x00" "true\0";
    static const char sFalse[] __attribute__ ((aligned (4))) = "\xff\xff\x05\x00" "false\0";

    StringData *to_string(int v)
    {
      if (v) {
        return (StringData*)(void*)sTrue;
      } else {
        return (StringData*)(void*)sFalse;
      }            
    }
  }

  // The proper StringData* representation is already laid out in memory by the code generator.
  uint32_t stringData(uint32_t lit)
  {
    return (uint32_t)getstr(lit);
  }


  namespace collection {

    RefCollection *mk(uint32_t flags)
    {
      RefCollection *r = new RefCollection(flags);
      return r;
    }

    int count(RefCollection *c) { return c->data.size(); }

    void add(RefCollection *c, uint32_t x) {
      if (c->flags & 1) incr(x);
      c->data.push_back(x);
    }

    inline bool in_range(RefCollection *c, int x) {
      return (0 <= x && x < (int)c->data.size());
    }

    uint32_t at(RefCollection *c, int x) {
      if (in_range(c, x)) {
        uint32_t tmp = c->data.at(x);
        if (c->flags & 1) incr(tmp);
        return tmp;
      }
      else {
        error(ERR_OUT_OF_BOUNDS);
        return 0;
      }
    }

    void remove_at(RefCollection *c, int x) {
      if (!in_range(c, x))
        return;

      if (c->flags & 1) decr(c->data.at(x));
      c->data.erase(c->data.begin()+x);
    }

    void set_at(RefCollection *c, int x, uint32_t y) {
      if (!in_range(c, x))
        return;

      if (c->flags & 1) {
        decr(c->data.at(x));
        incr(y);
      }
      c->data.at(x) = y;
    }

    int index_of(RefCollection *c, uint32_t x, int start) {
      if (!in_range(c, start))
        return -1;

      if (c->flags & 2) {
        StringData *xx = (StringData*)x;
        for (uint32_t i = start; i < c->data.size(); ++i) {
          StringData *ee = (StringData*)c->data.at(i);
          if (xx->len == ee->len && memcmp(xx->data, ee->data, xx->len) == 0)
            return (int)i;
        }
      } else {
        for (uint32_t i = start; i < c->data.size(); ++i)
          if (c->data.at(i) == x)
            return (int)i;
      }

      return -1;
    }

    int remove(RefCollection *c, uint32_t x) {
      int idx = index_of(c, x, 0);
      if (idx >= 0) {
        remove_at(c, idx);
        return 1;
      }

      return 0;
    }
  }

  namespace record {
    RefRecord* mk(int reflen, int totallen)
    {
      check(0 <= reflen && reflen <= totallen, ERR_SIZE, 1);
      check(reflen <= totallen && totallen <= 255, ERR_SIZE, 2);

      void *ptr = ::operator new(sizeof(RefRecord) + totallen * sizeof(uint32_t));
      RefRecord *r = new (ptr) RefRecord();
      r->len = totallen;
      r->reflen = reflen;
      memset(r->fields, 0, r->len * sizeof(uint32_t));
      return r;
    }
  }

  typedef uint32_t Action;

  namespace action {
    Action mk(int reflen, int totallen, int startptr)
    {
      check(0 <= reflen && reflen <= totallen, ERR_SIZE, 1);
      check(reflen <= totallen && totallen <= 255, ERR_SIZE, 2);
      check(bytecode[startptr] == 0xffff, ERR_INVALID_BINARY_HEADER, 3);
      check(bytecode[startptr + 1] == 0, ERR_INVALID_BINARY_HEADER, 4);


      uint32_t tmp = (uint32_t)&bytecode[startptr];

      if (totallen == 0) {
        return tmp; // no closure needed
      }

      void *ptr = ::operator new(sizeof(RefAction) + totallen * sizeof(uint32_t));
      RefAction *r = new (ptr) RefAction();
      r->len = totallen;
      r->reflen = reflen;
      r->func = (ActionCB)((tmp + 4) | 1);
      memset(r->fields, 0, r->len * sizeof(uint32_t));

      return (Action)r;
    }

    void run(Action a)
    {
      if (hasVTable(a))
        ((RefAction*)a)->run();
      else {
        check(*(uint16_t*)a == 0xffff, ERR_INVALID_BINARY_HEADER, 4);
        ((void (*)())((a + 4) | 1))();
      }
    }
  }


  // ---------------------------------------------------------------------------
  // Implementation of the BBC micro:bit features
  // ---------------------------------------------------------------------------
  using namespace touch_develop;

  namespace bitvm_micro_bit {

    // -------------------------------------------------------------------------
    // Helpers
    // -------------------------------------------------------------------------

    void callback(MicroBitEvent e, Action a) {
      action::run(a);
    }


    // -------------------------------------------------------------------------
    // Pins
    // -------------------------------------------------------------------------

    void onPinPressed(int pin, Action a) {
      if (a != 0) {
        incr(a);
        // Forces the PIN to switch to makey-makey style detection.
        switch(pin) {
          case MICROBIT_ID_IO_P0:
            uBit.io.P0.isTouched();
            break;
          case MICROBIT_ID_IO_P1:
            uBit.io.P1.isTouched();
            break;
          case MICROBIT_ID_IO_P2:
            uBit.io.P2.isTouched();
            break;
        }
        uBit.MessageBus.ignore(
          pin,
          MICROBIT_BUTTON_EVT_CLICK,
          (void (*)(MicroBitEvent, void*)) callback);
        uBit.MessageBus.listen(
          pin,
          MICROBIT_BUTTON_EVT_CLICK,
          (void (*)(MicroBitEvent, void*)) callback,
          (void*) a);
      }
    }

    // -------------------------------------------------------------------------
    // Buttons
    // -------------------------------------------------------------------------

    void onButtonPressedExt(int button, int event, Action a) {
      if (a != 0) {
        incr(a);
        uBit.MessageBus.ignore(
          button,
          event,
          (void (*)(MicroBitEvent, void*)) callback);
        uBit.MessageBus.listen(
          button,
          event,
          (void (*)(MicroBitEvent, void*)) callback,
          (void*) a);
      }
    }

    void onButtonPressed(int button, Action a) {
      onButtonPressedExt(button, MICROBIT_BUTTON_EVT_CLICK, a);
    }


    // -------------------------------------------------------------------------
    // System
    // -------------------------------------------------------------------------
    
    void fiberDone(void *a)
    {
      decr((Action)a);
      release_fiber();
    }


    void runInBackground(Action a) {
      if (a != 0) {
        incr(a);
        create_fiber((void(*)(void*))action::run, (void*)a, fiberDone);
      }
    }

    void forever_stub(void *a) {
      while (true) {
        action::run((Action)a);
        micro_bit::pause(20);
      }
    }

    void forever(Action a) {
      if (a != 0) {
        incr(a);
        create_fiber(forever_stub, (void*)a);
      }
    }

    // -------------------------------------------------------------------------
    // Images (helpers that create/modify a MicroBitImage)
    // -------------------------------------------------------------------------
    
    // Argument rewritten by the code emitter to be what we need
    ImageData *createImage(uint32_t lit) {
      return MicroBitImage(getbytes(lit)).clone().leakData();
    }

    ImageData *createReadOnlyImage(uint32_t lit) {
      return getbytes(lit);
    }

    ImageData *createImageFromString(StringData *s) {
      return ::touch_develop::micro_bit::createImageFromString(ManagedString(s)).leakData();
    }

    ImageData *displayScreenShot()
    {
      return uBit.display.screenShot().leakData();
    }
    
    ImageData *imageClone(ImageData *i)
    {
      return MicroBitImage(i).clone().leakData();
    }

    void clearImage(ImageData *i) {
      MicroBitImage(i).clear();
    }

    int getImagePixel(ImageData *i, int x, int y) {
      return MicroBitImage(i).getPixelValue(x, y);
    }

    void setImagePixel(ImageData *i, int x, int y, int value) {
      MicroBitImage(i).setPixelValue(x, y, value);
    }

    int getImageHeight(ImageData *i) {
      return i->height;
    }

    int getImageWidth(ImageData *i) {
      return i->width;
    }

    bool isImageReadOnly(ImageData *i) {
      return i->isReadOnly();
    }

    // -------------------------------------------------------------------------
    // Various "show"-style functions to display and scroll things on the screen
    // -------------------------------------------------------------------------

    void showLetter(StringData *s) {
      ::touch_develop::micro_bit::showLetter(ManagedString(s));
    }

    void scrollString(StringData *s, int delay) {
      ::touch_develop::micro_bit::scrollString(ManagedString(s), delay);
    }

    void showImage(ImageData *i, int offset) {
      ::touch_develop::micro_bit::showImage(MicroBitImage(i), offset);
    }

    void scrollImage(ImageData *i, int offset, int delay) {
      ::touch_develop::micro_bit::scrollImage(MicroBitImage(i), offset, delay);
    }

    void plotImage(ImageData *i, int offset) {
      ::touch_develop::micro_bit::plotImage(MicroBitImage(i), offset);
    }

    // [lit] argument is rewritted by code emitted
    void showLeds(uint32_t lit, int delay) {
      uBit.display.print(MicroBitImage(getbytes(lit)), 0, 0, 0, delay);
    }

    void plotLeds(uint32_t lit) {
      plotImage(getbytes(lit), 0);
    }

    void showAnimation(uint32_t lit, int ms) {
      uBit.display.animate(MicroBitImage(getbytes(lit)), ms, 5, 0);
    }

    // -------------------------------------------------------------------------
    // Functions that expose member fields of objects because the compilation 
    // scheme only works with the C-style calling convention 
    // -------------------------------------------------------------------------

    void compassCalibrateEnd() { uBit.compass.calibrateEnd(); }
    void compassCalibrateStart() { uBit.compass.calibrateStart(); }
    void reset() { uBit.reset(); }

    MicroBitPin *ioP0() { return &uBit.io.P0; }
    MicroBitPin *ioP1() { return &uBit.io.P1; }
    MicroBitPin *ioP2() { return &uBit.io.P2; }
    MicroBitPin *ioP3() { return &uBit.io.P3; }
    MicroBitPin *ioP4() { return &uBit.io.P4; }
    MicroBitPin *ioP5() { return &uBit.io.P5; }
    MicroBitPin *ioP6() { return &uBit.io.P6; }
    MicroBitPin *ioP7() { return &uBit.io.P7; }
    MicroBitPin *ioP8() { return &uBit.io.P8; }
    MicroBitPin *ioP9() { return &uBit.io.P9; }
    MicroBitPin *ioP10() { return &uBit.io.P10; }
    MicroBitPin *ioP11() { return &uBit.io.P11; }
    MicroBitPin *ioP12() { return &uBit.io.P12; }
    MicroBitPin *ioP13() { return &uBit.io.P13; }
    MicroBitPin *ioP14() { return &uBit.io.P14; }
    MicroBitPin *ioP15() { return &uBit.io.P15; }
    MicroBitPin *ioP16() { return &uBit.io.P16; }
    MicroBitPin *ioP19() { return &uBit.io.P19; }
    MicroBitPin *ioP20() { return &uBit.io.P20; }

    void panic(int code)
    {
      uBit.panic(code);
    }

    void serialSendString(StringData *s)
    {
      uBit.serial.sendString(ManagedString(s));
    }

    StringData *serialReadString()
    {
      return uBit.serial.readString().leakData();
    }
    
    void serialSendImage(ImageData *img)
    {
      uBit.serial.sendImage(MicroBitImage(img));
    }

    ImageData *serialReadImage(int width, int height)
    {
      return uBit.serial.readImage(width, height).leakData();
    }

    void serialSendDisplayState() { uBit.serial.sendDisplayState(); }
    void serialReadDisplayState() { uBit.serial.readDisplayState(); }
  }


  void error(ERROR code, int subcode)
  {
    printf("Error: %d [%d]\n", code, subcode);
    die();
  }


  uint32_t *globals;
  int numGlobals;

  uint32_t *allocate(uint16_t sz)
  {
    uint32_t *arr = new uint32_t[sz];
    memset(arr, 0, sz * 4);
    return arr;
  }

  void exec_binary(uint16_t *pc)
  {
    printf("start!\n");

    // XXX re-enable once the calibration code is fixed and [editor/embedded.ts]
    // properly prepends a call to [internal_main].
    // ::touch_develop::internal_main();

    uint32_t ver = *pc++;
    check(ver == V5BINARY, ERR_INVALID_BINARY_HEADER);
    numGlobals = *pc++;
    globals = allocate(numGlobals);
    pc += 6; // reserved

    uint32_t startptr = (uint32_t)pc;
    startptr |= 1; // Thumb state
    startptr = ((uint32_t (*)())startptr)();
    printf("stop main\n");

#ifdef DEBUG_MEMLEAKS
    bitvm::debugMemLeaks();
#endif

    return;
  }
  

  // The ARM Thumb generator in the JavaScript code is parsing
  // the hex file and looks for the magic numbers as present here.
  //
  // Then it fetches function pointer addresses from there.
  //
  // The code generator will assert if the Touch Develop function
  // has different number of input/output parameters than the one
  // defined here.
  //
  // It of course should match the C++ implementation.
  
  const uint32_t functionsAndBytecode[16000] __attribute__((aligned(0x20))) = {
    // Magic header to find it in the file
    0x08010801, 0x42424242, 0x08010801, 0x8de9d83e, 
    // List of pointers generated by scripts/functionTable.js
    #include "build/pointers.inc"
  };


}

// vim: ts=2 sw=2 expandtab
