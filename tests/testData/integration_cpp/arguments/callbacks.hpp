#ifndef kni_test_integration_arguments_callbacks_h
#define kni_test_integration_arguments_callbacks_h

#ifdef __cplusplus
extern "C" {
#endif

int callfn1(int (*cb)(int));

int callfn2(int (*cb)(int,const char*));

struct cbwrapper {
    int (*cb1)(int);
    int (*cb2)(int, const char*);
};

int callwrapper(cbwrapper * w);

#ifdef __cplusplus
}
#endif

#endif // kni_test_integration_arguments_callbacks_h
