#pragma once
/** @file
    Conversions between std::string (UTF-8) and Foundation strings and data. Apple
    only.
*/


#import <Foundation/Foundation.h>
#include <string>

namespace eacp::Strings
{
NSData* toNSData(const std::string& input);
NSString* toNSString(const std::string& input);

/// The error's localizedDescription; empty for nil.
std::string toStdString(NSError* error);
std::string toStdString(NSData* data);
std::string toStdString(NSString* string);
} // namespace eacp::Strings