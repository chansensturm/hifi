//
//  JurisdictionMap.cpp
//  libraries/octree/src
//
//  Created by Brad Hefta-Gaub on 8/1/13.
//  Copyright 2013 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include <QtCore/QSettings>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QDebug>

#include <PacketHeaders.h>
#include <OctalCode.h>

#include "JurisdictionMap.h"


// standard assignment
// copy assignment 
JurisdictionMap& JurisdictionMap::operator=(const JurisdictionMap& other) {
    copyContents(other);
    return *this;
}

#ifdef HAS_MOVE_SEMANTICS
// Move constructor
JurisdictionMap::JurisdictionMap(JurisdictionMap&& other) : _rootOctalCode(NULL) {
    init(other._rootOctalCode, other._endNodes);
    other._rootOctalCode = NULL;
    other._endNodes.clear();
}

// move assignment
JurisdictionMap& JurisdictionMap::operator=(JurisdictionMap&& other) {
    init(other._rootOctalCode, other._endNodes);
    other._rootOctalCode = NULL;
    other._endNodes.clear();
    return *this;
}
#endif

// Copy constructor
JurisdictionMap::JurisdictionMap(const JurisdictionMap& other) : _rootOctalCode(NULL) {
    copyContents(other);
}

void JurisdictionMap::copyContents(unsigned char* rootCodeIn, const std::vector<unsigned char*>& endNodesIn) {
    unsigned char* rootCode;
    std::vector<unsigned char*> endNodes;
    if (rootCodeIn) {
        size_t bytes = bytesRequiredForCodeLength(numberOfThreeBitSectionsInCode(rootCodeIn));
        rootCode = new unsigned char[bytes];
        memcpy(rootCode, rootCodeIn, bytes);
    } else {
        rootCode = new unsigned char[1];
        *rootCode = 0;
    }
    
    for (size_t i = 0; i < endNodesIn.size(); i++) {
        if (endNodesIn[i]) {
            size_t bytes = bytesRequiredForCodeLength(numberOfThreeBitSectionsInCode(endNodesIn[i]));
            unsigned char* endNodeCode = new unsigned char[bytes];
            memcpy(endNodeCode, endNodesIn[i], bytes);
            endNodes.push_back(endNodeCode);
        }
    }
    init(rootCode, endNodes);
}

void JurisdictionMap::copyContents(const JurisdictionMap& other) {
    _nodeType = other._nodeType;
    copyContents(other._rootOctalCode, other._endNodes);
}

JurisdictionMap::~JurisdictionMap() {
    clear();
}

void JurisdictionMap::clear() {
    if (_rootOctalCode) {
        delete[] _rootOctalCode;
        _rootOctalCode = NULL;
    }
    
    for (size_t i = 0; i < _endNodes.size(); i++) {
        if (_endNodes[i]) {
            delete[] _endNodes[i];
        }
    }
    _endNodes.clear();
}

JurisdictionMap::JurisdictionMap(NodeType_t type) : _rootOctalCode(NULL) {
    _nodeType = type;
    unsigned char* rootCode = new unsigned char[1];
    *rootCode = 0;
    
    std::vector<unsigned char*> emptyEndNodes;
    init(rootCode, emptyEndNodes);
}

JurisdictionMap::JurisdictionMap(const char* filename) : _rootOctalCode(NULL) {
    clear(); // clean up our own memory
    readFromFile(filename);
}

JurisdictionMap::JurisdictionMap(unsigned char* rootOctalCode, const std::vector<unsigned char*>& endNodes)  
    : _rootOctalCode(NULL) {
    init(rootOctalCode, endNodes);
}

void myDebugoutputBits(unsigned char byte, bool withNewLine) {
    if (isalnum(byte)) {
        printf("[ %d (%c): ", byte, byte);
    } else {
        printf("[ %d (0x%x): ", byte, byte);
    }
    
    for (int i = 0; i < 8; i++) {
        printf("%d", byte >> (7 - i) & 1);
    }
    printf(" ] ");
    
    if (withNewLine) {
        printf("\n");
    }
}


void myDebugPrintOctalCode(const unsigned char* octalCode, bool withNewLine) {
    if (!octalCode) {
        printf("NULL");
    } else {
        for (size_t i = 0; i < bytesRequiredForCodeLength(numberOfThreeBitSectionsInCode(octalCode)); i++) {
            myDebugoutputBits(octalCode[i],false);
        }
    }
    if (withNewLine) {
        printf("\n");
    }
}


JurisdictionMap::JurisdictionMap(const char* rootHexCode, const char* endNodesHexCodes) {

    qDebug("JurisdictionMap::JurisdictionMap(const char* rootHexCode=[%p] %s, const char* endNodesHexCodes=[%p] %s)",
        rootHexCode, rootHexCode, endNodesHexCodes, endNodesHexCodes);

    _rootOctalCode = hexStringToOctalCode(QString(rootHexCode));

    qDebug("JurisdictionMap::JurisdictionMap() _rootOctalCode=%p octalCode=", _rootOctalCode);
    myDebugPrintOctalCode(_rootOctalCode, true);
    
    QString endNodesHexStrings(endNodesHexCodes);
    QString delimiterPattern(",");
    QStringList endNodeList = endNodesHexStrings.split(delimiterPattern);

    for (int i = 0; i < endNodeList.size(); i++) {
        QString endNodeHexString = endNodeList.at(i);

        unsigned char* endNodeOctcode = hexStringToOctalCode(endNodeHexString);
        
        qDebug("JurisdictionMap::JurisdictionMap()  endNodeList(%d)=%s",
            i, endNodeHexString.toLocal8Bit().constData());
        
        //printOctalCode(endNodeOctcode);
        _endNodes.push_back(endNodeOctcode);

        qDebug("JurisdictionMap::JurisdictionMap() endNodeOctcode=%p octalCode=", endNodeOctcode);
        myDebugPrintOctalCode(endNodeOctcode, true);

    }    
}


void JurisdictionMap::init(unsigned char* rootOctalCode, const std::vector<unsigned char*>& endNodes) {
    clear(); // clean up our own memory
    _rootOctalCode = rootOctalCode;
    _endNodes = endNodes;
}

JurisdictionMap::Area JurisdictionMap::isMyJurisdiction(const unsigned char* nodeOctalCode, int childIndex) const {
    // to be in our jurisdiction, we must be under the root...

    // if the node is an ancestor of my root, then we return ABOVE
    if (isAncestorOf(nodeOctalCode, _rootOctalCode)) {
        return ABOVE;
    }
    
    // otherwise...
    bool isInJurisdiction = isAncestorOf(_rootOctalCode, nodeOctalCode, childIndex);
    // if we're under the root, then we can't be under any of the endpoints
    if (isInJurisdiction) {
        for (size_t i = 0; i < _endNodes.size(); i++) {
            bool isUnderEndNode = isAncestorOf(_endNodes[i], nodeOctalCode);
            if (isUnderEndNode) {
                isInJurisdiction = false;
                break;
            }
        }
    }
    return isInJurisdiction ? WITHIN : BELOW;
}


bool JurisdictionMap::readFromFile(const char* filename) {
    QString     settingsFile(filename);
    QSettings   settings(settingsFile, QSettings::IniFormat);
    QString     rootCode = settings.value("root","00").toString();
    qDebug() << "rootCode=" << rootCode;

    _rootOctalCode = hexStringToOctalCode(rootCode);
    printOctalCode(_rootOctalCode);

    settings.beginGroup("endNodes");
    const QStringList childKeys = settings.childKeys();
    QHash<QString, QString> values;
    foreach (const QString &childKey, childKeys) {
        QString childValue = settings.value(childKey).toString();
        values.insert(childKey, childValue);
        qDebug() << childKey << "=" << childValue;

        unsigned char* octcode = hexStringToOctalCode(childValue);
        printOctalCode(octcode);
        
        _endNodes.push_back(octcode);
    }
    settings.endGroup();
    return true;
}

void JurisdictionMap::displayDebugDetails() const {
    QString rootNodeValue = octalCodeToHexString(_rootOctalCode);

    qDebug() << "root:" << rootNodeValue;
    
    for (size_t i = 0; i < _endNodes.size(); i++) {
        QString value = octalCodeToHexString(_endNodes[i]);
        qDebug() << "End node[" << i << "]: " << rootNodeValue;
    }
}


bool JurisdictionMap::writeToFile(const char* filename) {
    QString     settingsFile(filename);
    QSettings   settings(settingsFile, QSettings::IniFormat);


    QString rootNodeValue = octalCodeToHexString(_rootOctalCode);

    settings.setValue("root", rootNodeValue);
    
    settings.beginGroup("endNodes");
    for (size_t i = 0; i < _endNodes.size(); i++) {
        QString key = QString("endnode%1").arg(i);
        QString value = octalCodeToHexString(_endNodes[i]);
        settings.setValue(key, value);
    }
    settings.endGroup();
    return true;
}

int JurisdictionMap::packEmptyJurisdictionIntoMessage(NodeType_t type, unsigned char* destinationBuffer, int availableBytes) {
    unsigned char* bufferStart = destinationBuffer;
    
    int headerLength = populatePacketHeader(reinterpret_cast<char*>(destinationBuffer), PacketTypeJurisdiction);
    destinationBuffer += headerLength;

    // Pack the Node Type in first byte
    memcpy(destinationBuffer, &type, sizeof(type));
    destinationBuffer += sizeof(type);

    // No root or end node details to pack!
    int bytes = 0;
    memcpy(destinationBuffer, &bytes, sizeof(bytes));
    destinationBuffer += sizeof(bytes);
    
    return destinationBuffer - bufferStart; // includes header!
}

int JurisdictionMap::packIntoMessage(unsigned char* destinationBuffer, int availableBytes) {
    unsigned char* bufferStart = destinationBuffer;
    
    int headerLength = populatePacketHeader(reinterpret_cast<char*>(destinationBuffer), PacketTypeJurisdiction);
    destinationBuffer += headerLength;

    // Pack the Node Type in first byte
    NodeType_t type = getNodeType();
    memcpy(destinationBuffer, &type, sizeof(type));
    destinationBuffer += sizeof(type);

    // add the root jurisdiction
    if (_rootOctalCode) {
        size_t bytes = bytesRequiredForCodeLength(numberOfThreeBitSectionsInCode(_rootOctalCode));
        memcpy(destinationBuffer, &bytes, sizeof(bytes));
        destinationBuffer += sizeof(bytes);
        memcpy(destinationBuffer, _rootOctalCode, bytes);
        destinationBuffer += bytes;
        
        // if and only if there's a root jurisdiction, also include the end nodes
        int endNodeCount = _endNodes.size(); 
        memcpy(destinationBuffer, &endNodeCount, sizeof(endNodeCount));
        destinationBuffer += sizeof(endNodeCount);

        for (int i=0; i < endNodeCount; i++) {
            unsigned char* endNodeCode = _endNodes[i];
            size_t bytes = 0;
            if (endNodeCode) {
                bytes = bytesRequiredForCodeLength(numberOfThreeBitSectionsInCode(endNodeCode));
            }
            memcpy(destinationBuffer, &bytes, sizeof(bytes));
            destinationBuffer += sizeof(bytes);
            memcpy(destinationBuffer, endNodeCode, bytes);
            destinationBuffer += bytes;
        }
    } else {
        int bytes = 0;
        memcpy(destinationBuffer, &bytes, sizeof(bytes));
        destinationBuffer += sizeof(bytes);
    }
    
    return destinationBuffer - bufferStart; // includes header!
}

int JurisdictionMap::unpackFromMessage(const unsigned char* sourceBuffer, int availableBytes) {
    clear();
    const unsigned char* startPosition = sourceBuffer;

    // increment to push past the packet header
    int numBytesPacketHeader = numBytesForPacketHeader(reinterpret_cast<const char*>(sourceBuffer));
    sourceBuffer += numBytesPacketHeader;
    int remainingBytes = availableBytes - numBytesPacketHeader;
    
    // read the root jurisdiction
    int bytes = 0;
    memcpy(&bytes, sourceBuffer, sizeof(bytes));
    sourceBuffer += sizeof(bytes);
    remainingBytes -= sizeof(bytes);

    if (bytes > 0 && bytes <= remainingBytes) {
        _rootOctalCode = new unsigned char[bytes];
        memcpy(_rootOctalCode, sourceBuffer, bytes);
        sourceBuffer += bytes;
        remainingBytes -= bytes;
        
        // if and only if there's a root jurisdiction, also include the end nodes
        int endNodeCount = 0;
        memcpy(&endNodeCount, sourceBuffer, sizeof(endNodeCount));
        sourceBuffer += sizeof(endNodeCount);
        for (int i=0; i < endNodeCount; i++) {
            int bytes = 0;
            memcpy(&bytes, sourceBuffer, sizeof(bytes));
            sourceBuffer += sizeof(bytes);
            remainingBytes -= sizeof(bytes);
            
            if (bytes <= remainingBytes) {
                unsigned char* endNodeCode = new unsigned char[bytes];
                memcpy(endNodeCode, sourceBuffer, bytes);
                sourceBuffer += bytes;
                remainingBytes -= bytes;
            
                // if the endNodeCode was 0 length then don't add it
                if (bytes > 0) {
                    _endNodes.push_back(endNodeCode);
                }
            }
        }
    }
    
    return sourceBuffer - startPosition; // includes header!
}
