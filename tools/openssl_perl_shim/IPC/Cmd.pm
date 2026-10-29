# 极简 IPC::Cmd 替代品: 只有 can_run() 被用到。
#
# 为什么需要它: OpenSSL 的 Configure 与 util/perl/OpenSSL/config.pm 都写了
# `use IPC::Cmd`, 但真正用到的只有 can_run()。完整的 IPC::Cmd 还会连带 require
# Params::Check 与 ExtUtils::MakeMaker, 而 Windows 上现成的 perl (Git for Windows
# 自带的那份) 没有这些模块 —— 于是 Configure 在一开始就 die。
#
# 目录约定: 本目录由 tools/deps_openssl.ps1 拷到 OpenSSL 源码树里, 用相对的
# PERL5LIB 传给 perl (相对路径不会碰到 msys perl 把 "C:\..." 的冒号当分隔符的问题)。
package IPC::Cmd;

use strict;
use warnings;
use Exporter 'import';

our @EXPORT_OK = qw(can_run);
our $VERSION   = '0.01';

# 找一个可执行文件, 找不到返回 undef
sub can_run {
    my ($command) = @_;
    return undef unless defined $command && length $command;

    # 带目录部分的写法 (绝对路径或含分隔符) 直接判断文件本身
    if ($command =~ m{[\\/]} || $command =~ m{^[A-Za-z]:}) {
        return $command if -f $command;
        return undef;
    }

    require File::Spec;
    for my $dir (File::Spec->path()) {
        next unless defined $dir && length $dir;
        # Windows 上的可执行文件可能带扩展名; 其余后缀在 Unix 上不会命中, 无害
        for my $suffix ('', '.exe', '.cmd', '.bat') {
            my $full = File::Spec->catfile($dir, $command . $suffix);
            return $full if -f $full;
        }
    }
    return undef;
}

1;
