
#include "CInode.h"
#include "CDir.h"
#include "CDentry.h"

#include "MDS.h"
#include "include/Message.h"

#include "messages/MInodeSyncStart.h"
#include "messages/MExportDir.h"

#include <string>

#include "include/config.h"
#undef dout
#define dout(x)  if (x <= g_conf.debug) cout << "cinode:"



// ====== CInode =======
CInode::CInode() : LRUObject() {
  ref = 0;
  
  parent = NULL;
  nparents = 0;
  lru_next = lru_prev = NULL;
  
  dir_auth = CDIR_AUTH_PARENT;
  dir = NULL;  // create CDir as needed

  auth_pins = 0;
  nested_auth_pins = 0;

  state = 0;
  dist_state = 0;
  lock_active_count = 0;
  
  pending_sync_request = 0;

  version = 0;

  auth = true;  // by default.
}

CInode::~CInode() {
  if (dir) { delete dir; dir = 0; }
}

CDir *CInode::get_parent_dir()
{
  if (parent)
	return parent->dir;
  return NULL;
}
CInode *CInode::get_parent_inode() 
{
  if (parent) 
	return parent->dir->inode;
  return NULL;
}



void CInode::make_path(string& s)
{
  if (parent) {
	parent->dir->inode->make_path(s);
	s += "/";
	s += parent->name;
  } 
  else if (is_root()) {
	s = "";  // root
  } 
  else {
	s = "(dangling)";  // dangling
  }
}

ostream& operator<<(ostream& out, CInode& in)
{
  string path;
  in.make_path(path);
  return out << "[" << in.inode.ino << " " << path << " " << &in << "]";
}


void CInode::hit(int type)
{
  assert(type >= 0 && type < MDS_NPOP);
  popularity[type].hit();

  // hit my containing directory, too
  //if (parent) parent->dir->hit();
}


void CInode::mark_dirty() {
  
  dout(10) << "mark_dirty " << *this << endl;

  // touch my private version
  version++;
  if (!(state & CINODE_STATE_DIRTY)) {
	state |= CINODE_STATE_DIRTY;
	get(CINODE_PIN_DIRTY);
  }
  
  // relative to parent dir:
  if (parent) {
	// dir is now dirty (if it wasn't already)
	parent->dir->mark_dirty();
	
	// i now live in that (potentially newly dirty) version
	parent_dir_version = parent->dir->get_version();
  }
}


// state 

crope CInode::encode_export_state()
{
  crope r;
  Inode_Export_State_t istate;

  istate.inode = inode;
  istate.version = version;
  istate.popularity = popularity[0]; // FIXME all pop values?
  //istate.ref = in->ref;
  istate.ncached_by = cached_by.size();
  
  istate.is_softasync = is_softasync();
  assert(!is_syncbyme());
  assert(!is_lockbyme());
  
  if (is_dirty())
	istate.dirty = true;
  else istate.dirty = false;

  if (is_dir()) 
	istate.dir_auth = dir_auth;
  else
	istate.dir_auth = -1;

  // append to rope
  r.append( (char*)&istate, sizeof(istate) );
  
  // cached_by
  for (set<int>::iterator it = cached_by.begin();
	   it != cached_by.end();
	   it++) {
	int i = *it;
	r.append( (char*)&i, sizeof(int) );
  }

  return r;
}

crope CInode::encode_basic_state()
{
  crope r;

  // inode
  r.append((char*)&inode, sizeof(inode));
  
  // cached_by
  int n = cached_by.size();
  r.append((char*)&n, sizeof(int));
  for (set<int>::iterator it = cached_by.begin(); 
	   it != cached_by.end();
	   it++) {
	int j = *it;
	r.append((char*)&j, sizeof(j));
  }

  // dir_auth
  r.append((char*)&dir_auth, sizeof(int));
  
  return r;
}
 
int CInode::decode_basic_state(crope r, int off)
{
  // inode
  r.copy(0,sizeof(inode_t), (char*)&inode);
  off += sizeof(inode_t);
	
  // cached_by --- although really this is rep_by,
  //               since we're non-authoritative
  int n;
  r.copy(off, sizeof(int), (char*)&n);
  off += sizeof(int);
  cached_by.clear();
  for (int i=0; i<n; i++) {
	int j;
	r.copy(off, sizeof(int), (char*)&j);
	cached_by.insert(j);
	off += sizeof(int);
  }

  // dir_auth
  r.copy(off, sizeof(int), (char*)&dir_auth);
  off += sizeof(int);

  return off;
}



// waiting

bool CInode::is_frozen()
{
  if (parent && parent->dir->is_frozen())
	return true;
  return false;
}

bool CInode::is_freezing()
{
  if (parent && parent->dir->is_freezing())
	return true;
  return false;
}

void CInode::add_waiter(int tag, Context *c) {
  // waiting on hierarchy?
  if (tag & CDIR_WAIT_ATFREEZEROOT && (is_freezing() || is_frozen())) {  
	parent->dir->add_waiter(tag, c);
	return;
  }
  
  // this inode.
  if (waiting.size() == 0)
	get(CINODE_PIN_WAITER);
  waiting.insert(pair<int,Context*>(tag,c));
  dout(10) << "add_waiter " << tag << " " << c << " on inode " << *this << endl;
}

void CInode::take_waiting(int mask, list<Context*>& ls)
{
  if (waiting.empty()) return;
  
  multimap<int,Context*>::iterator it = waiting.begin();
  while (it != waiting.end()) {
	if (it->first & mask) {
	  ls.push_back(it->second);
	  dout(10) << "take_waiting mask " << mask << " took " << it->second << " tag " << it->first << " on inode " << *this << endl;
	  waiting.erase(it++);
	} else {
	  dout(10) << "take_waiting mask " << mask << " SKIPPING " << it->second << " tag " << it->first << " on inode " << *this << endl;
	  it++;
	}
  }

  if (waiting.empty())
	put(CINODE_PIN_WAITER);
}



// auth_pins
bool CInode::can_auth_pin() {
  if (parent)
	return parent->dir->can_auth_pin();
  return true;
}

void CInode::auth_pin() {
  get(CINODE_PIN_IAUTHPIN + auth_pins);
  auth_pins++;
  dout(7) << "auth_pin on inode " << *this << " count now " << auth_pins << " + " << nested_auth_pins << endl;
  if (parent)
	parent->dir->adjust_nested_auth_pins( 1 );
}

void CInode::auth_unpin() {
  auth_pins--;
  dout(7) << "auth_unpin on inode " << *this << " count now " << auth_pins << " + " << nested_auth_pins << endl;
  put(CINODE_PIN_IAUTHPIN + auth_pins);
  if (parent)
	parent->dir->adjust_nested_auth_pins( -1 );
}



// authority

int CInode::authority(MDCluster *cl) {
  if (parent == NULL)
	return 0;  // i am root
  return parent->dir->dentry_authority( parent->name, cl );
}


int CInode::dir_authority(MDCluster *mdc) 
{
  // explicit
  if (dir_auth >= 0) {
	dout(11) << "dir_auth explicit " << dir_auth << " at " << *this << endl;
	return dir_auth;
  }

  // parent
  if (dir_auth == CDIR_AUTH_PARENT) {
	dout(11) << "dir_auth parent at " << *this << endl;
	return authority(mdc);
  }
}




void CInode::add_parent(CDentry *p) {
  nparents++;
  if (nparents == 1)         // first
	parent = p;
  else if (nparents == 2) {  // second, switch to the vector
	parents.push_back(parent);
	parents.push_back(p);
  } else                     // additional
	parents.push_back(p);
}

void CInode::remove_parent(CDentry *p) {
  nparents--;
  if (nparents == 0) {         // first
	assert(parent == p);
	parent = 0;
  }
  else if (nparents == 1) {  // second, switch back from the vector
	parent = parents.front();
	if (parent == p)
	  parent = parents.back();
	assert(parent != p);
	parents.clear();
  } else {
	assert(0); // implement me
  }
}



void CInode::dump(int dep)
{
  string ind(dep, '\t');
  //cout << ind << "[inode " << this << "]" << endl;
  
  if (dir)
	dir->dump(dep);
}

void CInode::dump_to_disk(MDS *mds) 
{
  if (dir)
	dir->dump_to_disk(mds);
}


