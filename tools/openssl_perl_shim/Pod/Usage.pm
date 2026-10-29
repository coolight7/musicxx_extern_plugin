# 极简 Pod::Usage 替代品: 只有 pod2usage() 被用到。
#
# 为什么需要它: OpenSSL 生成的 configdata.pm 在文件顶部写了 `use Pod::Usage`
# (编译期就加载), 但只有跑 `perl configdata.pm --help` 之类才真正调用 pod2usage()。
# 完整模块要连带 Pod::Text 与 Pod::Simple, Git for Windows 自带的 perl 没有, 会让
# 每一次 `make` 里的 perl 调用 (生成头文件) 都以 "Can't locate Pod/Usage.pm" 失败,
# 表现是编译到一半的生成头文件全是 0 字节。
#
# 目录约定同 IPC/Cmd.pm: 由 tools/deps_openssl.ps1 拷进 OpenSSL 源码树, 用相对
# PERL5LIB 传进去。
package Pod::Usage;

use strict;
use warnings;
use Exporter 'import';

our @EXPORT    = qw(pod2usage);
our @EXPORT_OK = qw(podselect);
our $VERSION   = '0.01';

# 构建流程不会用到 podselect(), 给个空实现免得调用点报未定义
sub podselect {
    return '';
}

# 只打印消息与一行提示就退出; 不打印 POD 正文 (构建流程不需要)
sub pod2usage {
    my (%args) = @_;
    my $exit = defined $args{'-exitval'} ? $args{'-exitval'} : 2;
    my $msg  = $args{'-message'} // '';
    print STDERR $msg;
    print STDERR "usage: 详见该脚本的文档\n";
    exit($exit eq 'NOEXIT' ? 0 : $exit);
}

1;
