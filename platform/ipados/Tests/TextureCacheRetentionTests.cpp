#include <cassert>
#include <cstdio>
#include <initializer_list>
#include "Cafe/HW/Latte/Core/TextureCacheRetention.h"
int main() {
 using TextureCacheRetention::ShouldEvictRAMCopy;
 // Native and potentially dynamic/related copies preserve the old lifetime.
 for (bool constrained : {false, true}) {
  assert(!ShouldEvictRAMCopy(constrained,false,119999,10000));
  assert(!ShouldEvictRAMCopy(constrained,false,120000,1999));
  assert(ShouldEvictRAMCopy(constrained,false,120000,2000));
 }
 assert(!ShouldEvictRAMCopy(false,true,15000,450));
 assert(!ShouldEvictRAMCopy(true,true,14999,450));
 assert(!ShouldEvictRAMCopy(true,true,15000,449));
 assert(ShouldEvictRAMCopy(true,true,15000,450));
 // Paused loading alone does not qualify recent textures; both clocks matter.
 assert(!ShouldEvictRAMCopy(true,true,120000,2));
 puts("PASS: Native retention unchanged, constrained independent RAM copies expire after both age guards, dynamic/related copies protected");
}
