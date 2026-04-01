#include "logStream.h"
#include <DateTime.h>
#include "out/ConfigDB/app-log.h"

// Global instance

LogStream::LogStream()
{
    instance = this;
    Serial.println("LogStream: Initializing ConfigDB-based logging...");
}

LogStream::~logStream()
{

}

size_t logStream::write(uint8_t c)
{
    // Always write to Serial immediately
    Serial.write(c);
    
    // Buffer single characters until line completion
    if (c == '\n' || c == '\r') {
        // Process the completed line
        if (!lineBuffer.isEmpty()) {
            processLine();
        }
    } else if (c >= 32 && c <= 126) {  // Printable ASCII characters only
        lineBuffer += (char)c;
        
        // Force flush if line gets too long
        if (lineBuffer.length() >= maxLineLength) {
            processLine();
        }
    }
    // Ignore non-printable characters (except newlines)
    
    return 1;
}

size_t LogStream::write(const uint8_t* buffer, size_t size)
{
    // Memory protection - don't process if heap is critically low
    if (system_get_free_heap_size() < 5000) {
        // Still write to Serial
        Serial.write(buffer, size);
        return size;
    }
    
    // Process each character
    for (size_t i = 0; i < size; i++) {
        write(buffer[i]);
    }
    
    return size;
}

void LogStream::flush()
{
    // Flush any remaining content in buffer
    if (!lineBuffer.isEmpty()) {
        processLine();
    }
    Serial.flush();
}

void LogStream::processLine()
{
    if (lineBuffer.isEmpty()) {
        return;
    }
    
    // Clean up the line - remove trailing whitespace
    lineBuffer.trim();
    
    if (!lineBuffer.isEmpty()) {
        // Add to ConfigDB log with timestamp
        addLogEntry(lineBuffer);
    }
    
    // Clear the buffer
    lineBuffer = "";
}

void LogStream::addLogEntry(const String& message)
{
    // Memory protection - don't add log entries if memory is low
    if (system_get_free_heap_size() < 8000) {
        return;
    }
    
    // Check if the app.log database is available
    if (!app.log) {
        Serial.println("LogStream: Warning - app.log database not available");
        return;
    }
    
    try {
        // Get current timestamp
        DateTime now = SystemClock.now();
        int32_t timestamp = now.toUnixTime();
        
        // Access the log entries array
        AppLog::Root::LogEntries logEntries(*app.log);
        auto updater = logEntries.update();
        
        // Add new entry
        auto newEntry = updater.append();
        newEntry.setTimestamp(timestamp);
        newEntry.setMessage(message);
        
        // Commit the changes
        updater.commit();
        
        // Trim old entries if we have too many
        trimOldEntries();
        
    } catch (const std::exception& e) {
        Serial.printf("LogStream: Error adding log entry: %s\n", e.what());
    } catch (...) {
        Serial.println("LogStream: Unknown error adding log entry");
    }
}

void LogStream::trimOldEntries()
{
    if (!app.log) {
        return;
    }
    
    try {
        AppLog::Root::LogEntries logEntries(*app.log);
        size_t count = logEntries.size();
        
        if (count > maxLogEntries) {
            auto updater = logEntries.update();
            
            // Remove oldest entries (from the beginning)
            size_t toRemove = count - maxLogEntries;
            for (size_t i = 0; i < toRemove; i++) {
                updater.remove(0); // Always remove first element
            }
            
            updater.commit();
            Serial.printf("LogStream: Trimmed %u old log entries\n", toRemove);
        }
    } catch (const std::exception& e) {
        Serial.printf("LogStream: Error trimming log entries: %s\n", e.what());
    } catch (...) {
        Serial.println("LogStream: Unknown error trimming log entries");
    }
}

void LogStream::clearLog()
{
    if (!app.log) {
        Serial.println("LogStream: Warning - app.log database not available");
        return;
    }
    
    try {
        AppLog::Root::LogEntries logEntries(*app.log);
        auto updater = logEntries.update();
        updater.clear();
        updater.commit();
        Serial.println("LogStream: Log cleared");
    } catch (const std::exception& e) {
        Serial.printf("LogStream: Error clearing log: %s\n", e.what());
    } catch (...) {
        Serial.println("LogStream: Unknown error clearing log");
    }
}

size_t LogStream::getLogEntryCount()
{
    if (!app.log) {
        return 0;
    }
    
    try {
        AppLog::Root::LogEntries logEntries(*app.log);
        return logEntries.size();
    } catch (...) {
        return 0;
    }
}

void LogStream::printRecentEntries(Stream& output, size_t count)
{
    if (!app.log) {
        output.println("LogStream: Database not available");
        return;
    }
    
    try {
        AppLog::Root::LogEntries logEntries(*app.log);
        size_t totalEntries = logEntries.size();
        
        if (totalEntries == 0) {
            output.println("LogStream: No log entries");
            return;
        }
        
        // Calculate starting index
        size_t startIdx = 0;
        if (totalEntries > count) {
            startIdx = totalEntries - count;
        }
        
        output.printf("LogStream: Showing %u most recent entries (total: %u):\n", 
                     totalEntries - startIdx, totalEntries);
        
        for (size_t i = startIdx; i < totalEntries; i++) {
            auto entry = logEntries[i];
            DateTime dt;
            dt.fromUnixTime(entry.getTimestamp());
            
            output.printf("[%s] %s\n", 
                         dt.toShortTimeString().c_str(),
                         entry.getMessage().c_str());
        }
    } catch (const std::exception& e) {
        output.printf("LogStream: Error reading log entries: %s\n", e.what());
    } catch (...) {
        output.println("LogStream: Unknown error reading log entries");
    }
}