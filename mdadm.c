#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "mdadm.h"
#include "util.h"
#include "jbod.h"



int mount_state = 0; // 0 = unmounted, 1 = mounted
int address_max = 1048575; // for address boundary checking

uint32_t op_construct(int diskID, int blockID, int cmd, int reserved) {
  // helper function combines given fields into a 32-bit JBOD operation code in proper format
  uint32_t opcode = ((diskID << 28) | (blockID << 20) | (cmd << 14) | (reserved));
  return opcode;
}

int mdadm_mount(void) {
  /* YOUR CODE */

  // check disk is not currently mounted
  if (mount_state == 1) {
    return -1;
  }

  jbod_cmd_t mount_command = JBOD_MOUNT;
  uint32_t opcode = op_construct(0,0,mount_command,0);
  int status; // contain return value from jbod_op to track errors
  status = jbod_operation(opcode, NULL);
  if (status == 0) {
    mount_state = 1;
    return 1;
  }

  return -1;
}

int mdadm_unmount(void) {
  /* YOUR CODE */

  // check disk is currently mounted
  if (mount_state == 0) {
    return -1;
  }

  jbod_cmd_t unmount_command = JBOD_UNMOUNT;
  uint32_t opcode = op_construct(0,0,unmount_command,0);
  int status; // contain return value from jbod_op to track errors
  status = jbod_operation(opcode, NULL);
  if (status == 0) {
    mount_state = 0;
    return 1;
  }

  return -1;
}

// Helper function to seek to disk and block, returns blockOffset 
int seek(int addr) {
  int diskNumber = addr / JBOD_DISK_SIZE; // this should return a int value from 0-15
  int addressInDisk = addr % JBOD_DISK_SIZE; // gives address relative to beginning of disk space
  int localBlockNum = addressInDisk / JBOD_BLOCK_SIZE; // block ID in disk
  int blockOffset = addressInDisk % JBOD_BLOCK_SIZE; // offset within block

  /*
    The disk and block ID must be computed so that the jbod can seek to the
    correct locations to read/write. The offset within block is required so that
    the buffer can be proberly separated and aligned for reading/writing.
  */

  // Seek to correct diskID
  jbod_cmd_t command = JBOD_SEEK_TO_DISK;
  uint32_t opcode = op_construct(diskNumber, 0, command, 0);
  int status; // contain return value from jbod_op to track errors
  status = jbod_operation(opcode, NULL);
  if (status == -1) {
    return -1;
  }

  // Seek to correct blockID
  command = JBOD_SEEK_TO_BLOCK;
  opcode = op_construct(0, localBlockNum, command, 0);
  status = jbod_operation(opcode, NULL);
  if (status == -1) {
    return -1;
  }

  return blockOffset;
}

int mdadm_read(uint32_t addr, uint32_t len, uint8_t *buf) {
  /* YOUR CODE */

  // initial error checking structure
  if (mount_state == 0) { // check disk is mounted
    return -1;
  } else if (len > 1024) { // check max read length
    return -1;
  } else if (len == 0 && buf == NULL) {
    
  } else if ((addr + (len - 1)) > address_max) { // check address boundary
    return -1;
  } else if (len != 0 && buf == NULL) {  // check NULL pointer and non-zero length
    return -1;
  }

  int command = JBOD_READ_BLOCK;
  int opcode = op_construct(0, 0, command, 0);
  int status;

  int dataSize;
  int currentAddr = addr;
  int finalAddr = addr + len; // ensures correct # of blocks are read

  uint8_t *bufP = buf;  // allow changes to buffer reference point without altering original buf
  while (currentAddr < finalAddr) {

    int blockOffset = seek(currentAddr);
    if (blockOffset == -1) {
      return -1;
    }

    uint8_t loader[JBOD_BLOCK_SIZE]; 
    // this loader array will be where the jbod read outputs to. it will then be loaded into buf

    status = jbod_operation(opcode, loader); // this reads the first block that has desired data
    if (status == -1) {
      return -1;
    }
    /*
      After reading the first block that has desired data, data must be parsed from this block
      Then, next block can be read and parsed accordingly
      This will continue until all required blocks have been read
    */

   if (currentAddr == addr) { // first block read
    if (blockOffset + len <= JBOD_BLOCK_SIZE) { // all data is contained in this block
      dataSize = len;
      memcpy(bufP, loader + blockOffset, dataSize);

    } else {
      dataSize = JBOD_BLOCK_SIZE - blockOffset;
      memcpy(bufP, loader + blockOffset, dataSize);
    }
    currentAddr += dataSize;
    
   } else if (finalAddr - currentAddr >= JBOD_BLOCK_SIZE) { // at least a full block remaining
    bufP += dataSize;
    dataSize = JBOD_BLOCK_SIZE;
    memcpy(bufP, loader, dataSize);
    currentAddr += dataSize;

   } else { // read final block
    bufP += dataSize;
    dataSize = finalAddr - currentAddr;
    memcpy(bufP, loader, dataSize);
    currentAddr += dataSize;
   }
  }
  
  return len;
}

int mdadm_write(uint32_t addr, uint32_t len, const uint8_t *buf) {

  // initial error checking structure
  if (mount_state == 0) { // check disk is mounted
    return -1;
  } else if (len > 1024) { // check max write length
    return -1;
  } else if (len == 0 && buf == NULL) {
    
  } else if ((addr + (len - 1)) > address_max) { // check address boundary
    return -1;
  } else if (len != 0 && buf == NULL) {  // check NULL pointer and non-zero length
    return -1;
  }

  int command = JBOD_WRITE_BLOCK;
  int opcode = op_construct(0, 0, command, 0);
  int status;

  int dataSize;
  int currentAddr = addr;
  int finalAddr = addr + len; // ensures correct # of blocks are written
  
  const uint8_t *bufP = buf;  // allow changes to buffer reference point without altering original buf
  while (currentAddr < finalAddr) {

    int blockOffset = seek(currentAddr);
    if (blockOffset == -1) {
      return -1;
    }

    uint8_t loader[JBOD_BLOCK_SIZE];
    /* this loader array will contain the data to be written to jbod block.
     it will be filled with data from buf, and ensure only correct data is overwritten in jbod */

    // check potential writing starts
    if (blockOffset == 0) { // write starts aligned with block
      // check write amounts -- either write less than a full block or write an entire block
      if ((finalAddr - currentAddr) < JBOD_BLOCK_SIZE) {
        // ! CASE 1: write less than a full block amount, with 0 offset
        dataSize = finalAddr - currentAddr;
        mdadm_read(currentAddr, JBOD_BLOCK_SIZE, loader); // fill loader with current JBOD data
        memcpy(loader, bufP, dataSize); // fill beginning of loader with data to write

      } else {
        // ! CASE 2: write an entire full block
        dataSize = JBOD_BLOCK_SIZE;
        memcpy(loader, bufP, dataSize); // fill entire loader with data to write
      }

    } else {  // writing starts within block
      // check write amounts -- either until block end or middle segment of a block
      if ((blockOffset + len) > JBOD_BLOCK_SIZE) { 
        // ! CASE 3: write until block end, with offset
        dataSize = JBOD_BLOCK_SIZE - blockOffset;
        mdadm_read(currentAddr - blockOffset, JBOD_BLOCK_SIZE, loader); // fill loader with current JBOD data
        memcpy(loader + blockOffset, bufP, dataSize); // fill loader at offset position 

      } else {
        // ! CASE 4: write some middle segment of block
        dataSize = len;
        mdadm_read(currentAddr - blockOffset, JBOD_BLOCK_SIZE, loader); // fill loader with current JBOD data
        memcpy(loader + blockOffset, bufP, dataSize); // fill loader at offset position
      }

    }
    // return to correct position (since data was read from JBOD to fill loader)
    blockOffset = seek(currentAddr);
    if (blockOffset == -1) {
      return -1;
    }

    // adjust buffer position and current address
    bufP += dataSize;
    currentAddr += dataSize;

    // write to JBOD with appropriate data in loader
    status = jbod_operation(opcode, loader);
    if (status == -1) {
      return -1;
    }
  }
  
  return len;
}

/*CWD /home/ethan/CMPSC311/Lab3/sp24-lab3-EthanBloom */
/*CWD /home/ethan/CMPSC311/Lab3/sp24-lab3-EthanBloom */
/*CWD /home/ethan/CMPSC311/Lab3/sp24-lab3-EthanBloom */
/*CWD /home/ethan/CMPSC311/Lab3/sp24-lab3-EthanBloom */
/*CWD /home/ethan/CMPSC311/Lab3/sp24-lab3-EthanBloom */
/*CWD /home/ethan/CMPSC311/Lab3/sp24-lab3-EthanBloom */
/*CWD /home/ethan/CMPSC311/Lab3/sp24-lab3-EthanBloom */
/*CWD /home/ethan/CMPSC311/Lab3/sp24-lab3-EthanBloom */
/*CWD /home/ethan/CMPSC311/Lab3/sp24-lab3-EthanBloom */
/*CWD /home/ethan/CMPSC311/Lab3/sp24-lab3-EthanBloom */
/*CWD /home/ethan/CMPSC311/Lab3/sp24-lab3-EthanBloom */
/*CWD /home/ethan/CMPSC311/Lab3/sp24-lab3-EthanBloom */
/*CWD /home/ethan/CMPSC311/Lab3/sp24-lab3-EthanBloom */
/*CWD /home/ethan/CMPSC311/Lab3/sp24-lab3-EthanBloom */
