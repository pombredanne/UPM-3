
package Term::Size::Perl::Params; 

# created Mon Oct  2 07:33:06 2017

use vars qw($VERSION);
$VERSION = 0.029;

=head1 Term::Size::Perl::Params

=head2 params

    $href = Term::Size::Perl::Params

The configuration parameters C<Term::Size::Perl> needs to
know for retrieving the terminal size with C<ioctl>.

=cut

sub params {
    return (
        winsize => {
            sizeof => 8,
            mask => 'S!S!S!S!'
        },
        TIOCGWINSZ => {
            value => 21523,
            definition => qq{0x5413}
        }
    );
}

1;