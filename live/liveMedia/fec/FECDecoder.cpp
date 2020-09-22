#include "include/FECDecoder.hh"
#include <iostream>
#include <bitset>

unsigned FECDecoder::totalRecoveredPackets = 0;


FECDecoder* FECDecoder::createNew() {
    return new FECDecoder();
}

FECDecoder::FECDecoder() {}

void FECDecoder::repairCluster(RTPPacket** cluster, unsigned d, unsigned l, unsigned ssrc) {
	//std::cout << "CLUSTER TO REPAIR:" << "\n";
	//printCluster(cluster, d, l);

	unsigned numRecoveredUntilThisIteration = 0;
	unsigned numRecoveredSoFar = 0;

	while (true) {
        repairNonInterleaved(cluster, d, l, ssrc, &numRecoveredSoFar);
        repairInterleaved(cluster, d, l, ssrc, &numRecoveredSoFar);

		if (numRecoveredSoFar > numRecoveredUntilThisIteration) {
			numRecoveredUntilThisIteration = numRecoveredSoFar;
		}
		else break;
	}

    totalRecoveredPackets += numRecoveredSoFar;

    //std::cout << totalRecoveredPackets << "\n";

	//std::cout << "AFTER REPAIR:" << "\n";
	//printCluster(cluster, d, l);
}

RTPPacket* FECDecoder::repairRow(RTPPacket** row, unsigned size, unsigned ssrc, unsigned d, unsigned l) {
    //printRow(row, size);

    u_int16_t seqnum = findSequenceNumber(row, size, d, l);
    unsigned char* bitString = calculateRowBitString(row, size);
    unsigned char* header = createHeader(bitString, seqnum, ssrc);
    u_int16_t y = ((u_int16_t)bitString[8] << 8) | bitString[9];
    unsigned char* payload = calculatePayload(row, size, y);

    unsigned packetSize = RTP_HEADER_SIZE + y;
    unsigned char* content = new unsigned char[packetSize];
    for (unsigned i = 0; i < 12; i++) content[i] = header[i];
    for (unsigned i = 0; i < packetSize - 12; i++) content[i + 12] = payload[i];

    RTPPacket* rtpPacket = RTPPacket::createNew(content, packetSize);
    return rtpPacket;
}

unsigned char* FECDecoder::generateBitString(unsigned char* rtpPacket, unsigned payloadSize) {
    unsigned char* bitString = new unsigned char[10];

    for (int i = 0; i < 8; i++)
        bitString[i] = rtpPacket[i];

    uint16_t totalSize = 0;

    int CC = EXTRACT_BIT_RANGE(3, 0, rtpPacket[0]);
    totalSize += CC;

    Boolean hasExtensionHeader = EXTRACT_BIT(4, rtpPacket[0]) == 1 ? True : False;
    if (hasExtensionHeader) {
        int extensionSizePosition = RTP_HEADER_SIZE + CC * 4 + EXTENSION_HEADER_ID_SIZE;
        totalSize += ((u_int16_t)rtpPacket[extensionSizePosition] << 8) | rtpPacket[extensionSizePosition + 1]; //extension
    }

    Boolean hasPadding = EXTRACT_BIT(5, rtpPacket[0]) == 1 ? True : False;
    if (hasPadding) totalSize += rtpPacket[RTP_HEADER_SIZE + payloadSize - 1];

    totalSize += payloadSize;

    bitString[8] = totalSize >> 8;
    bitString[9] = totalSize;

    return bitString;
}

unsigned char* FECDecoder::generateFECBitString(unsigned char* buffer, unsigned size) {
    unsigned char* fecBitString = new unsigned char[10];

    fecBitString[0] = buffer[0 + 12];
    fecBitString[1] = buffer[1 + 12];
    for (int i = 4; i < 8; i++)
        fecBitString[i] = buffer[i + 12];

    fecBitString[8] = buffer[2 + 12];
    fecBitString[9] = buffer[3 + 12];

    return fecBitString;
}

/*what if we cant find seqnum? this will then return 0 which is a valid seqnum*/
/*Does this work for interleaved packets???????????????? because we increase by one and not row????*/
u_int16_t FECDecoder::findSequenceNumber(RTPPacket** row, unsigned rowSize, unsigned d, unsigned l) {
    RTPPacket* fecPacket = row[rowSize - 1];
    int payload = EXTRACT_BIT_RANGE(0, 7, fecPacket->content()[1]);

    u_int16_t base = (((u_int16_t)fecPacket->content()[20]) << 8) | fecPacket->content()[21];
    for (unsigned i = 0; i < rowSize; i++) {
        if (row[i] == NULL) {
            if (payload == 115) return base + i;
            else if (payload == 116) return base + i * l;
        }
    }
    return 0;
}

//We do fec packet first, becuase it must be present.
unsigned char* FECDecoder::calculateRowBitString(RTPPacket** row, unsigned rowSize) {
    unsigned char* bitString = generateFECBitString(row[rowSize - 1]->content(), row[rowSize - 1]->size());

    for (unsigned i = 0; i < rowSize - 1; i++) {
        RTPPacket* current = row[i];
        if (current == NULL) continue;
        unsigned char* nextBitString = generateBitString(current->content(), current->size() - 12);
        for (int j = 0; j < 10; j++)
            bitString[j] ^= nextBitString[j];
    }
    return bitString;
}

unsigned char* FECDecoder::createHeader(unsigned char* bitString, u_int16_t sequenceNumber, unsigned ssrc) {
    unsigned char* header = new unsigned char[12];

    header[0] = 0b10000000 | (bitString[0] & 0b00111111); //Set version 2, add padd, extension and cc
    header[1] = bitString[1]; //set marker, payload type
    header[2] = sequenceNumber >> 8; //set sequence number
    header[3] = sequenceNumber; //set sequence number
    for (int i = 4; i < 8; i++)
        header[i] = bitString[i]; //set ts recovery

    header[8] = ssrc >> 24;
    header[9] = ssrc >> 16;
    header[10] = ssrc >> 8;
    header[11] = ssrc;

    return header;
}

//We do fec first because it must be present
unsigned char* FECDecoder::calculatePayload(RTPPacket** row, unsigned rowSize, u_int16_t Y) {
    unsigned char* payload = new unsigned char[Y];

    RTPPacket* fecPacket = row[rowSize - 1];
    for (int i = 0; i < Y; i++)
        payload[i] = (i + 24) < fecPacket->size() ? fecPacket->content()[i + 24] : 0x00;

    for (int i = 0; i < (rowSize - 1); i++) {
        RTPPacket* current = row[i];
        if (current == NULL) continue;
        for (int j = 0; j < Y; j++)
            payload[j] ^= (j + 12) < current->size() ? current->content()[j + 12] : 0x00;
    }

    return payload;
}

void FECDecoder::repairNonInterleaved(RTPPacket** cluster, unsigned d, unsigned l, unsigned ssrc, unsigned* numRecoveredSoFar) {
    unsigned rowSize = l + 1;
    for (int i = 0; i < rowSize * d; i += rowSize) {
        int fecIndex = i + rowSize - 1;

        int missingPacketCount = 0;
        for (int j = i; j <= fecIndex; j++) {
            if (cluster[j] == NULL) missingPacketCount++;
        }
        if (missingPacketCount != 1 || cluster[fecIndex] == NULL) continue; //we dont care if the fec packet is missing

        RTPPacket** row = new RTPPacket*[rowSize];
        for (int j = 0; j < rowSize; j++) row[j] = NULL;
        for (int j = 0; j < rowSize; j++) {
            row[j] = cluster[i + j];
        }

        RTPPacket* repairedPacket = repairRow(row, rowSize, ssrc, d, l);
        delete[] row;

        for (int j = i; j <= fecIndex; j++) {
            if (cluster[j] == NULL) {
                cluster[j] = repairedPacket;
                break;
            }
        }
        (*numRecoveredSoFar)++;
    }
}

void FECDecoder::repairInterleaved(RTPPacket** cluster, unsigned d, unsigned l, unsigned ssrc, unsigned* numRecoveredSoFar) {
    unsigned rowSize = l + 1;
    for (int i = 0; i < l; i++) {
        int fecIndex = i + rowSize * d;

        int missingPacketCount = 0;

        for (int j = i; j < fecIndex; j += rowSize) {
            if (cluster[j] == NULL) missingPacketCount++;
        }
        if (missingPacketCount != 1 || cluster[fecIndex] == NULL) continue; //we dont care if the fec packet is missing

        unsigned columnSize = d + 1;
        RTPPacket** column = new RTPPacket*[columnSize];
        for (int j = 0; j < columnSize; j++) column[j] = NULL;
        for (int j = 0; j < columnSize; j++) {
            column[j] = cluster[i + j * rowSize];
        }

        RTPPacket* repairedPacket = repairRow(column, columnSize, ssrc, d, l);
        delete[] column;

        for (int j = i; j < fecIndex; j += rowSize) {
            if (cluster[j] == NULL) {
                cluster[j] = repairedPacket;
                break;
            }
        }
        (*numRecoveredSoFar)++;
    }
}

void FECDecoder::printCluster(RTPPacket** cluster, unsigned d, unsigned l) {
    int size = (d + 1) * (l + 1) - 1;
	for (int i = 0; i < size; i++) {
		RTPPacket* curr = cluster[i];
		if (curr == NULL) std::cout << "NULL ";
		else {
            int payload = EXTRACT_BIT_RANGE(0, 7, curr->content()[1]);
            u_int16_t seq = payload == 115 || payload == 116 ? extractFECBase(curr) : extractRTPSeq(curr);
            std::cout << seq << " ";
		}
		if ((i+1) % (l+1) == 0) std::cout << "\n";
	}
	std::cout << "\n\n";
}

void FECDecoder::printRow(RTPPacket** row, unsigned rowSize) {
    for (unsigned i = 0; i < rowSize; i++) {
        RTPPacket* curr = row[i];
        if (curr == NULL) std::cout << "NULL ";
        else {
            int payload = EXTRACT_BIT_RANGE(0, 7, curr->content()[1]);
            u_int16_t seq = payload == 115 || payload == 116 ? extractFECBase(curr) : extractRTPSeq(curr);
            std::cout << seq << " ";
        }
    }
    std::cout << "\n";
}

u_int16_t FECDecoder::extractFECBase(RTPPacket* rtpPacket) {
    return (((u_int16_t)rtpPacket->content()[20]) << 8) | rtpPacket->content()[21];
}

u_int16_t FECDecoder::extractRTPSeq(RTPPacket* rtpPacket) {
    return (((u_int16_t)rtpPacket->content()[2]) << 8) | rtpPacket->content()[3];
}

unsigned FECDecoder::extractSSRC(RTPPacket* rtpPacket) {
    return (unsigned)rtpPacket->content()[8] << 24 | (unsigned)rtpPacket->content()[9] << 16 | (unsigned)rtpPacket->content()[10] << 8 | (unsigned)rtpPacket->content()[11];
}
