// The Apple backend of include/port/http.hpp, for macOS, iOS and tvOS.
//
// NSURLSession uses the system's TLS stack and certificate store, so nothing
// has to be bundled. The request is asynchronous by nature; the caller is
// already on a worker thread, so this simply waits for it.

#import <Foundation/Foundation.h>

#include <stdlib.h>
#include <string.h>

static char* copy_utf8(NSString* text) {
    const char* utf8 = text.UTF8String;
    if (utf8 == NULL) {
        return NULL;
    }
    return strdup(utf8);
}

int PartyBoard_HttpPostApple(const char* url, const void* body, size_t bodyLength, const char* contentType,
    const char* userAgent, char** outBody, size_t* outLength, char** outError) {
    *outBody = NULL;
    *outLength = 0;
    *outError = NULL;

    @autoreleasepool {
        NSURL* target = [NSURL URLWithString:[NSString stringWithUTF8String:url]];
        if (target == nil) {
            *outError = strdup("invalid URL");
            return 0;
        }

        NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:target];
        request.HTTPMethod = @"POST";
        request.HTTPBody = [NSData dataWithBytes:body length:bodyLength];
        request.timeoutInterval = 30.0;
        [request setValue:[NSString stringWithUTF8String:contentType] forHTTPHeaderField:@"Content-Type"];
        [request setValue:[NSString stringWithUTF8String:userAgent] forHTTPHeaderField:@"User-Agent"];

        __block int status = 0;
        __block NSData* received = nil;
        __block NSString* failure = nil;
        dispatch_semaphore_t finished = dispatch_semaphore_create(0);

        NSURLSessionDataTask* task = [NSURLSession.sharedSession
            dataTaskWithRequest:request
              completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
                  if (error != nil) {
                      failure = error.localizedDescription;
                  } else {
                      if ([response isKindOfClass:[NSHTTPURLResponse class]]) {
                          status = (int)((NSHTTPURLResponse*)response).statusCode;
                      }
                      received = data;
                  }
                  dispatch_semaphore_signal(finished);
              }];
        [task resume];
        dispatch_semaphore_wait(finished, DISPATCH_TIME_FOREVER);

        if (failure != nil) {
            *outError = copy_utf8(failure);
            return 0;
        }
        if (received.length > 0) {
            *outBody = malloc(received.length);
            if (*outBody != NULL) {
                memcpy(*outBody, received.bytes, received.length);
                *outLength = received.length;
            }
        }
        return status;
    }
}
