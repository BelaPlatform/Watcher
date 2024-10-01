let Watcher = {
  sendCommand: (cmd, quiet) => {
    let obj = {
      watcher: Array.isArray(cmd) ? cmd : [ cmd ],
    };
    if(!quiet)
      console.log("Sending", obj);
    Bela.control.send(obj);
  },
  requestList: () => {
    Watcher.sendCommand({cmd: "list"});
  },
  backwCompatibility: true,
  backwTypes: [],
  watchers: [],
  processList: (watchers) => {
    this.backwTypes = watchers.map((v) => {
      return v.type;
    });
    this.watchers = watchers.map((v) => {
      return v.name;
    });
  },
  parseInputData: (buffers, list, useList) => {
    if(!buffers)
      return;
    let retBufs = Array();
    for(let k = 0; k < buffers.length; ++k)
    {
      if(useList) {
        // Bela.data.buffers have been populated only by the audio watched variables
        // everything else we have to set here
        retBufs.push({
          timestamp: list.timestamp,
          buf: [ list[k].valueInput ],
          watcher: this.watchers[k],
        });
      }
      if(!buffers[k])
        continue;
      let timestampBuf;
      let type = buffers[k].type;
      if(!type) {
        // when running with old version of the core GUI, type has to be set elsewhere
        backwCompatibility = true;
        type = this.backwTypes[k];
      }
      switch(type)
      {
        case 'c':
          // absurb reverse mapping of an absurd fwd mapping
          let intArr = buffers[k].slice(0, 8).map((e) => {
            return e.charCodeAt(0);
          });
          timestampBuf = new Uint8Array(intArr);
          buf = buffers[k].slice(8);
          break;
        case 'j': // unsigned int
          timestampBuf = new Uint32Array(buffers[k].slice(0, 2))
          buf = buffers[k].slice(2);
          break;
        case 'i': // int
          timestampBuf = new Int32Array(buffers[k].slice(0, 2))
          buf = buffers[k].slice(2);
          break;
        case 'f': // float
          timestampBuf = new Float32Array(buffers[k].slice(0, 2));
          buf = buffers[k].slice(2);
          break;
        case 'd':
          timestampBuf = new Float64Array(buffers[k].slice(0, 1));
          buf = buffers[k].slice(1);
          break;
        default:
          console.log("Unknown buffer type ", type);
          continue;
      }
      let timestampUint32 = new Uint32Array(timestampBuf.buffer);
      let timestamp = timestampUint32[0] * (1 << 32) + timestampUint32[1];
      retBufs.push({
        timestamp: timestamp,
        buf: buf,
        watcher: this.watchers[k],
      });
    }
    return retBufs;
  },
};
