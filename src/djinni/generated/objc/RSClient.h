// AUTOGENERATED FILE - DO NOT MODIFY!
// This file generated by Djinni from rocketspeed.djinni

#import "RSClient.h"
#import "RSConfiguration.h"
#import "RSMessageReceivedCallback.h"
#import "RSPublishCallback.h"
#import "RSSubscribeCallback.h"
#import <Foundation/Foundation.h>
@class RSMsgId;
@class RSNamespaceID;
@class RSSubscriptionPair;
@class RSTopic;
@class RSTopicOptions;

/**
 * The RocketSpeed Client object.
 * Implemented in c++ and called from Java and ObjC
 */

@protocol RSClient

+ (id <RSClient>)Open:(id <RSConfiguration>)config publishCallback:(id <RSPublishCallback>)publishCallback subscribeCallback:(id <RSSubscribeCallback>)subscribeCallback receiveCallback:(id <RSMessageReceivedCallback>)receiveCallback;

- (void)Publish:(RSTopic *)topicName namespaceId:(RSNamespaceID *)namespaceId options:(RSTopicOptions *)options data:(NSData *)data msgid:(RSMsgId *)msgid;

- (void)ListenTopics:(NSMutableArray *)names options:(RSTopicOptions *)options;

@end