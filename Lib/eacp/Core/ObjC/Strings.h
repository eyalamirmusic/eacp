#pragma once

#import <Foundation/Foundation.h>
#include <string>

namespace eacp::Strings
{
NSData* toNSData(const std::string& input);
NSString* toNSString(const std::string& input);

std::string toStdString(NSError* error);
std::string toStdString(NSData* data);
} // namespace eacp::Strings