#include <jni.h>
#include "Cards.h"

extern "C" JNIEXPORT jstring JNICALL
Java_com_discoveraroundme_home_MainActivity_nativeCardDescriptor(JNIEnv* env, jclass) {
  // Compile the real firmware descriptor header on Android. Registration,
  // scheduling, fetch and drawing remain future behavioral extraction work.
  Cards::CardSpec card;
  card.id = "forecast";
  card.kind = Cards::Kind::List;
  return env->NewStringUTF(card.id);
}
